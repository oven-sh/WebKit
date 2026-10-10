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
#include <wtf/SetForScope.h>

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
template<> struct ParameterOfOperation<AOT::Slot*> { using Type = Pointer; };

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
    // Rows of numbers that the code names by where they start: CompiledFunctionInfo::plans. See Lowering::rowOfNames().
    Vector<uint32_t> plans;
    // (slot, where its row starts): CompiledFunctionInfo::siteConstantIsPlan.
    Vector<std::pair<uint32_t, uint32_t>> rowsOfSlots;
    // The calls that a handler of the function watches, in the order of their numbers, each with the number of the handler.
    Vector<std::pair<uint32_t, uint32_t>> catchEntrypointsForCallSites;
    // By call-site number minus one: where the call is in the source, for stack traces.
    struct SitePosition {
        uint32_t position;
        uint32_t inlineFrame;
    };
    Vector<SitePosition> sitePositions;
    // By inline frame: the number that stands for the inlined call. No machine call has it. Entry 0 is unused.
    Vector<uint32_t> inlineCallSites;
    // The numbers from 1 that Lowering::run() gives out ahead of the blocks: a position, and no return address: the calls that were inlined.
    uint32_t numberOfSitesWithoutCode { 0 };
    unsigned numberOfCatchEntrypoints { 0 };
    // See useFaultsForNilTests().
    struct NilTest {
        ThrowKind kind;
        uint32_t a;
        std::optional<uint32_t> b;
        uint32_t callSite;
    };
    Vector<NilTest> nilTests;
    RefPtr<B3::StackmapGenerator> nilTestMarker;
    struct FaultingAccess {
        CCallHelpers::Label instruction;
        GPRReg base;
        uint32_t nilTest;
    };
    Vector<FaultingAccess> faultingAccesses;
    // But for the calls that bring a frame of their own if the function has none: see Lowering::callEntry().
    bool emitsCalls { false };
    // The blocks of the function that it does so for.
    BitVector blocksThatEmitCalls;
    bool readsCallerFrame { false };

    bool hasNoFrame(B3::Air::Code& code) const
    {
        return !numberOfCatchEntrypoints && !code.frameSize() && !code.calleeSaveRegisterAtOffsetList().registerCount() && !emitsCalls && !readsCallerFrame;
    }

    void emitFrameTeardown(CCallHelpers& jit, B3::Air::Code& code) const
    {
        if (hasNoFrame(code))
            return;
        jit.emitRestore(code.calleeSaveRegisterAtOffsetList());
        if (code.frameSize())
            jit.emitFunctionEpilogue();
        else
            jit.emitFunctionEpilogueWithEmptyFrame();
    }
};

Vector<uint32_t, 3> throwSiteWords(ThrowKind, Rep, unsigned registerNumber, uint32_t a, std::optional<uint32_t> b);
// An access through nil hits memory that is never mapped, up to this many bytes from the pointer.
static constexpr int64_t reachOfFaultingAccess = 16000;
void useFaultsForNilTests(B3::Procedure&, LoweringContext&);

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
    // The function of that number, or a part of it.
    Lowering(B3::Procedure&, const Program&, FunctionID, const Function&, LoweringContext&);

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
        auto& type = m_program.at(m_function.at(id).type);
        return type.mayBeUndefinedOrNull() && !type.isUndefinedOrNull() && hasPassedNilTest(id) ? withoutNil(m_function.at(id).type) : type;
    }
    bool hasPassedNilTest(ValueID) const;
    const Type& withoutNil(TypeID) const;
    Operand get(ValueID);
    void define(ValueID, Operand);
    static LType b3TypeOf(Rep);
    LBasicBlock newColdBlock();
    LBasicBlock newUnlikelyBlock();
    std::span<const Target> successorsOf(const Terminator&) const;
    Vector<BlockID> reversePostorder() const;
    void computeLoopBlocks(const Vector<BlockID>& order);
    void computeDominators(const Vector<BlockID>& order);
    struct Loop;
    void computeLoops(const Vector<BlockID>& order);
    bool isLoopInvariant(const Loop&, ValueID) const;
    bool isGuaranteedToExecute(const Loop&) const;
    bool willHoistComputationOn(ValueID) const;
    bool dominates(BlockID a, BlockID b) const { return m_dominatorTreeEnter[a.index()] <= m_dominatorTreeEnter[b.index()] && m_dominatorTreeLeave[b.index()] <= m_dominatorTreeLeave[a.index()]; }
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
    // Of a Branch on whether a reference is nil: which of the two targets is for nil.
    std::optional<unsigned> nilSuccessorOf(const Terminator&) const;
    // The value that a branch tests for `undefined` and `null`, and which of its successors is taken if it is neither.
    std::optional<std::pair<ValueID, unsigned>> nilTestOf(const Terminator&) const;
    LBasicBlock edgeTo(const Target&, bool isBranch);
    Operand lower(ValueID, const Value&);
    Operand lowerLanguageOperation(ValueID result, const Value&);
    CellPointer lowerNewPromise();
    void lowerResolvePromise(const Value&);

    template<typename Functor> void require(Word32 condition, const Functor& otherwise);
    template<typename T, typename Fast, typename Slow> T either(Word32 condition, const Fast&, const Slow&);
    // How the arms of a test come together. `isReference`: the value is boxed, and by its type a cell, `undefined` or `null`: see OperandJoin.
    struct Joined {
        Joined(Rep rep, bool isReference = false)
            : rep(rep)
            , isReference(isReference)
        {
        }

        Rep rep;
        bool isReference;
    };
    Joined joinedAs(const Type&) const;
    class OperandJoin;
    template<typename Fast, typename Slow> Operand eitherOperand(Word32 condition, Joined, const Fast&, const Slow&);
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
    Word32 isEmpty(Boxed a) { return Word32(m_out.isZero64(indexOrBits(a))); }
    Word32 isNotEmpty(Boxed a) { return Word32(m_out.notZero64(indexOrBits(a))); }

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
    Word32 naturalInt32OrElse(Number, LBasicBlock otherwise);
    Number remainder(ValueID dividend, ValueID divisor);
    Number fmodOffTheUsualPath(Number, Number);
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
    // Only where the value is a cell. Of a value from a 32-bit slot these are not the bits of the boxed value: what is no cell comes out as a pointer too.
    CellPointer asCell(Boxed value)
    {
        LValue cell = value.value();
        if (auto found = m_decoded.find(cell); found != m_decoded.end())
            cell = found->value.cell;
        m_takenForCells.add(cell);
        return CellPointer(cell);
    }
    Integer signExtended(Word32 value) { return Integer(m_out.signExt32To64(value.value())); }
    Integer zeroExtended(Word32 value) { return Integer(m_out.zeroExt(value.value(), B3::Int64)); }
    Word32 low32(Integer value) { return Word32(m_out.castToInt32(value.value())); }
    Word32 low32(Boxed value) { return Word32(m_out.castToInt32(value.value())); }
    Word32 toInt32(Number);
    Word32 toInt32(ValueID);
    Word32 toInt32OrElse(ValueID, LBasicBlock otherwise);
    std::optional<Word32> toInt32OfSum(ValueID);

    // Variables.
    TypedPointer addressOf(VariableID);
    void checkInitialized(Operand raw, StringID name);
    void findStoresToVariables();
    Operand lowerGetModuleVariable(ValueID result, const Value&);
    // Jumps to `miss` unless the GlobalPropertyCache holds for the global object as it is. Yields where the value is.
    TypedPointer globalPropertyOrElse(uint32_t cacheNumber, LBasicBlock miss);
    LValue wordOfGlobalPropertyCache(uint32_t cacheNumber, size_t offset);
    Boxed valueOfGlobalPropertyOrElse(uint32_t cacheNumber, LBasicBlock miss);
    Boxed lowerGetGlobal(const Value&);
    // Nothing: the base is not written as the global object's `this`.
    std::optional<Boxed> getPropertyOfGlobalThis(ValueID base, StringID name);
    void lowerPutGlobal(const Value&);
    void lowerPutModuleVariable(const Value&);
    Operand lowerGetEnvironmentField(ValueID result, const Value&);
    void lowerPutEnvironmentField(const Value&);

    // Tests and checks.
    Word32 isCell(Boxed value)
    {
        if (auto found = m_decoded.find(value.value()); found != m_decoded.end())
            return Word32(m_answerIsValue ? m_out.aboveOrEqual(found->value.loaded, m_out.constInt64(firstCellWord)) : m_out.isZero64(found->value.isNoCell));
        return Word32(Emitter::isCell(value.value()));
    }
    // Of a value of the type. What is neither a cell nor a number has JSValue::OtherTag, and no pointer has it: one bit to branch on.
    Word32 isCell(Boxed value, const Type& type) { return type.primitives & Primitive::Number || indexOf(value.value()) ? isCell(value) : Word32(m_out.testIsZero64(value.value(), m_out.constInt64(JSValue::OtherTag))); }
    Word32 isInt32(Boxed value) { return Word32(Emitter::isInt32(value.value())); }
    Word32 isNumber(Boxed value) { return Word32(Emitter::isNumber(value.value())); }
    Word32 isBoolean(Boxed value) { return Word32(Emitter::isBoolean(value.value())); }
    Word32 isUndefinedOrNull(Boxed value)
    {
        if (auto found = m_decoded.find(value.value()); found != m_decoded.end())
            return Word32(m_answerIsValue ? m_out.below(found->value.loaded, m_out.constInt64(firstCellWord)) : m_out.notZero64(found->value.isNoCell));
        return Word32(isOther(value.value()));
    }
    Word32 typeOfCell(CellPointer);
    Word32 add(Word32 a, Word32 b) { return binary<Word32>(&Output::add, a, b); }
    Word32 isZero(Word32 a) { return unary<Word32>(&Output::isZero32, a); }
    // The 32 bits every cell has at offsetOfShapeWord and at offsetOfOrderWord. The cell may be any object, string or symbol: see AOTTypedData.h.
    Word32 shapeWordOf(CellPointer);
    Word32 orderWordOf(CellPointer);
    Word32 passes(Word32 shapeWord, const ShapeTest&, LBasicBlock otherwise = nullptr);
    // A test, or several made one. With `bits`: a set of at most 64 shapes, from test.last() down, and test.set says nothing.
    struct MergedShapeTest {
        ShapeTest test;
        std::optional<uint64_t> bits;
    };
    Word32 passes(Word32 shapeWord, const MergedShapeTest&, LBasicBlock otherwise = nullptr);
    uint64_t bitsOf(const ShapeTest&) const;
    // Of which one has to pass.
    Vector<MergedShapeTest, 4> merged(Vector<ShapeTest, 4>) const;
    Word32 passesOneOf(Word32 shapeWord, const Vector<ShapeTest, 4>&, LBasicBlock otherwise = nullptr);
    Word32 passesOneOf(Word32 shapeWord, const Vector<LayoutID, 1>& views, LBasicBlock otherwise = nullptr);
    // Ends the block.
    void branchOnPassingOneOf(Word32 shapeWord, const Vector<ShapeTest, 4>&, FTL::WeightedTarget passing, FTL::WeightedTarget failing);
    Word32 lowerHasShapeOf(const Value&);
    Word32 lowerIsClassObjectAsDeclared(const Value&);
    Word32 isClosureOf(CellPointer, FunctionID, bool mustBeAsMade = false);
    Word32 lowerIsClosureOf(const Value&);
    Word32 functionOfClosure(CellPointer);
    void closureOfOrElse(CellPointer, FunctionID, bool mustBeAsMade, LBasicBlock otherwise);
    void branchOnClosureOf(const Value&, LBasicBlock isOne, LBasicBlock isNone);
    void findClosureTestsCarriedOutByBranches();

    // Holding values to types. A test reads the type and the shape word of a cell at most and writes nothing. Where one fails and a
    // TypeError is thrown, nothing comes back: so the back end folds a test that is repeated, and what it has loaded stays loaded.

    // The one way an operation makes sure that what it yields is of the type of its value. The operand is of the type `guarantee`, which is
    // what the operation vouches for by itself, in any representation that holds it. Yields it in repOf(type). Within the type: no code
    // but the conversion. The position of the TypeError is that of the value that is being lowered, which ends its block where a handler watches.
    Operand ensure(Operand, const Type& guarantee, const Type&);
    // Nothing is vouched for.
    Operand ensure(Boxed value, const Type& type) { return ensure(value, m_program.at(m_program.types.anything), type); }
    // The same for array[index] of a contiguous array. A plain-data element is converted on the slow path, stored back, and the read goes on.
    Operand ensureElement(CellPointer array, Integer index, Boxed element, const Type&);
    // Goes on where the operand, of type `from`, is of type `to`, with it in repOf(to). Where it is not, goes to the block.
    Operand fitOrElse(Operand, const Type& from, const Type& to, LBasicBlock otherwise);
    // The same as a boolean.
    Word32 fits(Operand, const Type& from, const Type& to);
    // Ends the block that is being appended to: nothing comes back.
    void throwTypeMismatch(Operand, const Type& from, const Type& to);
    // The receiver of an access or a call, which is cells and maybe `undefined` or `null`: the cell. For those two the TypeError of the language,
    // which names the property if there is a name. The test is isCell() of the value and its type, whoever asks.
    CellPointer nonNil(Operand, const Type&, StringID name);
    // A test that has passed, of the value or of what it is a cast of, on every way here, is not made again.
    CellPointer nonNil(ValueID receiver, StringID name);
    // Ends the block.
    // `accessed`: what is loaded from or stored to next where the value is a cell.
    void throwNullish(Boxed, StringID name, CellPointer accessed = { });
    // At the start of the arm of a test for nil that throws. True: whoever throws there appends to LoweringContext::nilTests.
    bool markNilTest(CellPointer accessed);
    // Options::validateAOTTypedFacts(). The operand is the value that has just been lowered.
    void validateFact(Operand, ValueID, const Type&);

    Word32 isCallable(CellPointer);
    Word32 lowerTypeOfIs(const Value&);
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
    // Options::auditAOTTypedAssertions().
    Operand lowerAuditedCast(ValueID result, const Value& cast);
    void auditResult(Operand, const Value&);
    Boxed assertionFailed(Operand, const Type& from, TypeID wanted, uint32_t what, ValueID storedTo = { });
    Word32 auditLineAndColumn();
    ValueID receiverOfNextStore(ValueID stored) const;
    Operand lowerCast(ValueID result, const Value&);
    Word32 typeAndReason(TypeID, CastReason);
    // Casts that are carried out in the arms of the read they come behind: see lowerFusedCast().
    unsigned numberOfUsesOf(ValueID);
    // Fields that are pinned to a string: see fieldIsPinned in AOTTypedData.h.
    struct PinnedRead {
        CellPointer cell;
        // Both as they were at the read.
        Word32 shapeWord;
        Word32 isAltered;
        LayoutID view;
        StringID name;
    };
    // Of the value at the current index: nothing is done with it but to ask whether it is this string or that, and on no way from here to there may a field be stored to.
    bool isOnlyComparedWithTextsBeforeAnyStore(ValueID read);
    TypedPointer guardOfPinnedFields(SlotID);
    Word32 isPinnedTo(const PinnedRead&, StringID text);
    // In front of a store to the field of an object that is of a member of `holder`, or of anything if that is none.
    void guardPinnedFields(LayoutID holder, const Field&, ValueID stored);
    bool mayFuseWithRead(size_t indexOfRead);
    unsigned numberOfCastsBehind(size_t index);
    unsigned numberOfCastsFusedWithRead(size_t indexOfRead);
    std::optional<size_t> indexOfUseFusedWithRead(size_t indexOfRead, unsigned numberOfCasts);
    unsigned numberOfCastsFusedBehindUse(size_t indexOfUse);
    struct ReadFusedWithUse {
        size_t indexOfRead;
        unsigned numberOfCasts;
    };
    std::optional<ReadFusedWithUse> readFusedWithUse(size_t indexOfUse);
    Operand lowerReadFusedWithUse(ReadFusedWithUse, size_t indexOfUse, unsigned numberOfCastsBehindUse);
    std::optional<Operand> lowerFusedCast(size_t index);
    std::optional<Operand> lowerFusedUse(size_t index);
    Operand carryOutFusedCasts(Operand, const Type& from);
    Operand placeholder(Rep);
    Operand thrown(Operand, const Type&);
    Boxed thrownNil(Boxed);
    // A written cast that leaves its work to the value right behind it, which does it where its own test fails: see sunkCoercionAt().
    struct SunkCoercion {
        ValueID cast;
        Vector<const ShapeTest*, 4> sources;
    };
    std::optional<SunkCoercion> sunkCoercionAt(size_t indexOfUse);
    bool coercionSinks(size_t indexOfCast);
    bool coercionIsOnlyReadThrough(size_t indexOfCast);
    bool usesCastTest(const Value& cast) const;
    // Ends the block.
    void branchOnTransitionSource(CellPointer, const SunkCoercion&, FTL::WeightedTarget isSource, FTL::WeightedTarget isNoSource);
    void carryOut(CellPointer, const SunkCoercion&);
    Operand lowerCastBehindCoercion(ValueID result, const Value&, const SunkCoercion&);
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
    Integer integerBound(Opcode, Number);
    std::optional<Word32> lowerRelationalWithIntegerBound(const Value&);
    Integer clampToMaximum(Integer, int64_t limit);
    Integer clampToMinimum(Integer, int64_t limit);
    Word32 lowerStrictEq(ValueID a, ValueID b);
    static bool toBooleanTakesStub(const Type&);
    Word32 isTruthy(CellPointer, const Type&);
    Word32 isTruthy(Boxed, const Type&);
    void findTruthinessPhis();
    void findLeavesFetchedLate();
    void findParametersJoinedInParts();
    bool isOnlyBranchedOn(ValueID);
    // A number that is compared with integer constants: the integer it is, or one that no constant is.
    unsigned numberOfComparisonsWithIntegers(ValueID);
    void didDefineNumber(ValueID, const Value&, Number);
    std::optional<Integer> integerOfNumber(ValueID);
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
    // The holder: the cell the slot is in. Only a slot without references can do without.
    Operand load(Store, TypedPointer, unsigned flags, LValue holder = nullptr);
    // Of a Store::OptionalCell32: the boxed value, from the word, zero-extended, and the cell it stands for if it stands for one.
    // `loaded`: the word as it was loaded, where `index` has `undefined` for absent.
    Boxed boxDecoded(LValue index, LValue cell, LValue loaded = nullptr);
    // The same value, made here from what was loaded. For a block that is rarely run: then nothing is made for it on the way there.
    Boxed remade(Boxed);
    // A boxed value that is a cell, `undefined` or `null`, as two values. The word: below firstCellWord where the value is no cell, and then its bits, or zero for `undefined` where
    // `zeroReadsUndefined`. The cell: the pointer, where the value is a cell. What is not kept in parts is both.
    struct Parts {
        LValue word;
        LValue cell;
        bool zeroReadsUndefined;
    };
    static constexpr int64_t firstCellWord = undefinedCell32 + 1;
    Parts partsOf(Boxed);
    Boxed fromParts(LValue word, LValue cell, bool zeroReadsUndefined);
    // Not zero: the word, of a slot that holds cells, `undefined`, `null` or nothing, is no cell. All but a cell are at most undefinedCell32. One value for both ways of asking: the back
    // end takes a branch on it and a branch on its being zero for one test, which it does not with a comparison and its negation.
    LValue isNoCell(LValue word) { return m_out.bitAnd(m_out.sub(word, m_out.constInt64(firstCellWord)), m_out.constInt64(std::numeric_limits<int64_t>::min())); }
    LValue indexOf(LValue boxed) const
    {
        auto found = m_decoded.find(boxed);
        return found == m_decoded.end() ? nullptr : found->value.index;
    }
    // What is no cell has the same bits in both.
    LValue indexOrBits(Boxed boxed) const
    {
        LValue index = indexOf(boxed.value());
        return index ? index : boxed.value();
    }
    LValue cellCageBase() { return marked(fixedPointer(Instance::offsetOfCellCageBase()), IsFixed | IsSafe); }
    LValue decodeCell32(LValue index) { return m_out.add(cellCageBase(), m_out.shl(index, m_out.constInt32(cell32Shift))); }
    // A row of Instance::typedClosures; the same as a pointer; what that is as long as the closure has not been asked for: the address of no cell, nor what any word of a 32-bit slot decodes to.
    LValue rowOfClosure(uint32_t general) { return m_out.load32(m_out.address(m_heaps.AOTTyped_runtimeState, fixedPointer(Instance::offsetOfTypedClosures()), static_cast<ptrdiff_t>(general) * sizeof(uint32_t))); }
    LValue closureIfMade(uint32_t general)
    {
        if (Options::useAOTWideReferences())
            return m_out.load64(m_out.address(m_heaps.AOTTyped_runtimeState, fixedPointer(Instance::offsetOfTypedClosures()), static_cast<ptrdiff_t>(general) * sizeof(void*)));
        return m_out.add(noClosure(), m_out.shl(m_out.zeroExtPtr(rowOfClosure(general)), m_out.constInt32(cell32Shift)));
    }
    LValue noClosure() { return Options::useAOTWideReferences() ? m_out.constInt64(0) : m_out.add(cellCageBase(), m_out.constInt64(1 << cell32Shift)); }
    // Where the cell comes out of a 32-bit slot the words are compared, both 32 bits wide, and where nothing else wants the cell it is not decoded. The State keeps the closure.
    Word32 isTheClosure(CellPointer cell, uint32_t general)
    {
        if (auto found = m_indexOfCell.find(cell.value()); found != m_indexOfCell.end())
            return Word32(m_out.equal(m_out.sub(m_out.castToInt32(found->value), m_out.int32One), rowOfClosure(general)));
        return equal(cell, CellPointer(closureIfMade(general)));
    }
    void validateCell32(LValue index, LValue stored, bool mayBeNil);
    void store(Store, Operand, TypedPointer);
    const B3::AbstractHeap& heapOf(SlotID slot) { return m_heaps.AOTTyped_slots[slot.index()]; }
    void barrier(CellPointer owner);
    bool isFollowedByStoreWithBarrier(ValueID environment) const;
    // A store of a cell to it has left its barrier to a later store. Until that one nothing may collect garbage: see applyEffects().
    ValueID m_environmentAwaitingBarrier;
    // Behind stores of the values into the elements of the array: see State::rememberElement().
    void barrier(CellPointer array, std::span<const Boxed> stored);
    // After the last store that makes a new cell what it is, before anyone else gets to see it.
    void fenceAfterInitialization();
    // After every initializing store: fences after the last one.
    void didLowerInitializingStore();
    // Options::validateAOTTypedFacts(): of the object that was allocated last, the fields that have no initial state because one of the values right behind the allocation stores to them, and that have not been stored to yet. As indexes into the fields of the layout.
    BitVector m_fieldsAwaitingFirstStore;
    // What is being emitted ends the process.
    bool m_isReportingThatFactDoesNotHold { false };
    LayoutID m_layoutOfFieldsAwaitingFirstStore;

    // Accesses.
    Word32 isAbsent(Operand raw);
    bool isLoweringInitializingStore() const { return isInitializingStore(m_program, m_function, *m_currentBlock, m_currentIndexInBlock); }
    // The field an access by name comes down to: the receiver is looked at as one type of the program, which has it. Null: none.
    const Field* fieldFor(const Type& receiver, StringID) const;
    const Field* fieldExpectedFor(const Type& expected, StringID) const;
    CellPointer withFieldOrElse(ValueID object, LayoutID view, const Field&, LBasicBlock otherwise, bool mayBeAbsent = false);
    bool hasMemberThatKeepsField(LayoutID view, StringID) const;
    bool hasMemberWithField(LayoutID view, StringID) const;
    bool mayBeFrozen(LayoutID view) const;
    // Of an object that has the field.
    Operand loadSlot(CellPointer, const Field&, unsigned flags);
    Operand readField(CellPointer, const Field&, const Type&);
    Operand readProperty(CellPointer, LayoutID view, const Field&, const Type&);
    Word32 hasField(CellPointer, const Field&);
    Word32 isFieldAbsent(CellPointer, const Field&, unsigned flags);
    void branchOnInt32InPlace(Integer signExtendedWord, FTL::WeightedTarget inPlace, FTL::WeightedTarget otherwise);
    // The initial stores that are carried out together with the one at the index of the current block. See lowerInitialStores().
    struct InitialStores {
        size_t first;
        unsigned count;
        uint32_t orderWord;
        // Of the function the stores are from. In a copy more may be known of the receiver, which is of this type all the same.
        const Type* typeOfThis;
    };
    std::optional<InitialStores> initialStoresAt(size_t index) const;
    void lowerInitialStores(const InitialStores&);
    // `{ a: x, b: y }` of an ordinary object: a NewObject, or an Object.create(null), and the DefineFields of its block that are carried out with it, by their indices in the block. See lowerObjectLiteral().
    struct ObjectLiteral {
        size_t allocation;
        bool hasNullPrototype;
        Vector<size_t, 6> fields;
        // The first of the slots of the site: two, and a third for a CreateThis.
        unsigned slot { 0 };
        // Of a CreateThis: its operands. The constructor that runs is new.target where only that is given.
        ValueID newTarget { };
        ValueID constructor { };
        // A descriptor that nobody sees but the Object.defineProperty() at this index of the block: it is not made.
        std::optional<size_t> describes;
        // That call, which stands here as a literal without fields: where the allocation of its descriptor is. See lowerPropertyDefinition().
        std::optional<size_t> descriptor;
    };
    bool isDefineProperty(const Value&) const;
    std::optional<size_t> definitionDescribedBy(const ObjectLiteral&);
    void lowerPropertyDefinition(const ObjectLiteral&);
    bool isDefineField(const Value&) const;
    // The accessor functions of a literal, defined one behind the other but for the constants and the closures that the definitions take. They are carried out in one call, where the last one stands.
    struct AccessorGroup {
        Vector<size_t, 8> members;
        LValue buffer { nullptr };
    };
    bool isAccessorDefinition(const Value&) const;
    AccessorGroup* accessorGroupAt(size_t index);
    void lowerMemberOfAccessorGroup(AccessorGroup&, const Value&);
    // False: the site has no slot, or the name is an index.
    bool emitDefineByName(Boxed object, StringID, Boxed value);
    bool isObjectCreateNull(const Value&) const;
    ObjectLiteral* objectLiteralAt(size_t index);
    // In an image that counts.
    CellPointer countPlainAllocation(PlainAllocationKind, ValueID, CellPointer);
    // `{ ...source, name: value, .. }`: the CloneObject, the stores to the copy behind it, and the cast that holds the copy to the type of the literal: lowered together where the last of them stands. By index in the block.
    struct CloneGroup {
        size_t clone;
        Vector<size_t, 4> stores { };
        size_t cast { notFound };
        // Those of layoutsClonedInPlace() for which every store is a plain store and the cast has nothing to do.
        Vector<LayoutID, 2> layouts { };
        // Something stands between the members that may throw, or read what the general way of copying changes. Then that way is taken where the CloneObject stands.
        bool isDecidedAtClone { false };
        // 0: it was, and `copied` is the copy. Else 1 + the index among `layouts`.
        LValue decision { nullptr };
        LValue copied { nullptr };
        size_t last() const { return cast != notFound ? cast : stores.last(); }
    };
    std::optional<CloneGroup> findCloneGroup(size_t indexOfClone);
    template<typename Functor> void testSourceOfCloneGroup(const CloneGroup&, LBasicBlock slowPath, const Functor& onLayout);
    void decideCloneGroup(CloneGroup&);
    Operand lowerCloneGroup(CloneGroup);
    CellPointer copyRecord(CellPointer, Word32 orderWord, const Layout&, const Vector<StringID, 4>& overwritten);
    std::optional<CloneGroup> m_cloneGroup;
    bool isExpectedToBeSymbol(const Function&, ValueID) const;
    void lowerObjectLiteral(const ObjectLiteral&);
    // 0 stays 0: the engine's default.
    static unsigned capacityThatFillsTheCell(unsigned inlineCapacity);
    // `stores`: a bit for each that is stored and not defined. Then `group` says where each stands.
    CellPointer newObjectWithProperties(unsigned slot, bool hasNullPrototype, ValueID newTarget, ValueID constructor, unsigned inlineCapacity, std::span<const StringID> names, std::span<const Boxed> values, uint64_t stores = 0, const ObjectLiteral* group = nullptr);
    CellPointer newObjectLiteral(std::span<const StringID> names, std::span<const Boxed> values);
    uint32_t rowOfNames(std::span<const StringID>);
    uint32_t rowOf(std::span<const uint32_t>);
    CellPointer cloneObject(ValueID source, bool keepsLayout);
    Vector<LayoutID, 2> layoutsClonedInPlace(const Type&) const;
    bool makesItsFirstArgument(const Value&) const;
    std::optional<size_t> indexOfUseThatMakes(ValueID) const;
    bool isLeftToItsFirstUse(ValueID id) const { return !!indexOfUseThatMakes(id); }
    // For that use to ask: a later use of the same kind finds the object made.
    bool isMadeHere(ValueID id) const { return indexOfUseThatMakes(id) == m_currentIndexInBlock; }
    void defineObjectMadeLater(ValueID allocation, CellPointer);
    // Stores by name to one array, in a row but for values that depend on nothing: `array.pos = -1; array.end = -1;`. By their indices in the current block. See lowerNamedStores().
    using NamedStores = Vector<size_t, 4>;
    const NamedStores* namedStoresAt(size_t index);
    void lowerNamedStores(const NamedStores&);
    // Stores to one object, of which nothing is known, by name or under a symbol, in a row but for values of which nobody can tell where they are run: `this.a = 0; this.b = []; this[kState] = null;`. See lowerPropertyRun().
    struct StoreInPropertyRun {
        // In the current block.
        size_t index;
        ValueID object;
        // One of the two.
        StringID name;
        ValueID key;
        ValueID stored;
        bool isDefinition;
    };
    struct PropertyRun {
        Vector<StoreInPropertyRun, 8> stores;
        // The first of the slots of the site: one, and one for each key; without keys a second one: see cachePropertyRunWithStorage().
        unsigned slot { 0 };
    };
    ValueID withoutUpcasts(ValueID) const;
    std::optional<StoreInPropertyRun> storeInPropertyRunAt(size_t index);
    bool maySeparateStoresOfPropertyRun(size_t index);
    const PropertyRun* propertyRunAt(size_t index);
    void lowerPropertyRun(const PropertyRun&);
    Operand readInt32OrElsewhere(CellPointer, const Field&, const Type&);
    bool isStoredInPlace(const Field&, ValueID stored) const;
    std::optional<uint32_t> fieldInitializedBy(LayoutID, const Value& store) const;
    bool collectorMayHaveRunSinceBirth(ValueID object) const;
    void storeInt32OrElsewhere(CellPointer, const Field&, ValueID stored, LBasicBlock otherwise = nullptr);
    bool isBornWith(const Allocation&, StringID name) const;
    void didCreateFieldInCell(CellPointer, const Field&, OperationEffects, const ScopedLambda<void()>& unlessAsked);
    void writeField(CellPointer, ValueID object, const Field&, ValueID stored);
    bool isAllocatedAsMemberOf(ValueID object, LayoutID view) const;
    // Of an object that is looked at as the view says: one of its members may have no such field, and where the view admits foreign objects
    // it may be any object. `lacks` is for all that is not known to have the field.
    template<typename Has, typename Lacks> Operand throughView(CellPointer, ValueID object, LayoutID view, const Field&, Joined, const Has&, const Lacks&);
    // What goes by the members of the view: `inPlace`. Where the view admits foreign objects, that is only for an object of a member, and
    // `generally` asks any other.
    template<typename InPlace, typename Generally> Operand ifOfMember(CellPointer, ValueID object, LayoutID view, Joined, const InPlace&, const Generally&);
    template<typename Passes, typename Fails> Operand byShape(CellPointer, const ShapeTest&, Joined, const Passes&, const Fails&, bool mostlyFails = false);
    bool mostMembersLack(LayoutID view, const Field&) const;
    Operand lowerGetField(ValueID result, const Value&);
    void lowerPutField(ValueID, const Value&);
    Operand lowerGetById(ValueID result, const Value&);
    void lowerPutById(ValueID, const Value&);
    Word32 lowerInById(const Value&);
    Word32 emitInByVal(Boxed object, Boxed key);
    Operand lowerNewObject(ValueID, const Value&);
    CellPointer cellInFrame(size_t);
    // Reloaded at every use: the run time replaces the extension when it creates a field or makes a shape transition.
    CellPointer extensionOf(CellPointer cell) { return loadField<CellPointer>(cell, m_heaps.AOTTypedObject_extension); }
    TypedPointer fieldAt(CellPointer cell, const Field& field) { return m_out.address(heapOf(field.slot), (field.isOutOfLine ? extensionOf(cell) : cell).value(), field.offset); }

    // Calls.
    uint32_t nextSite()
    {
        if (m_currentCatchEntrypoint)
            m_context.catchEntrypointsForCallSites.append({ m_nextSite, *m_currentCatchEntrypoint });
        // Before the first block: the prologue. A terminator goes by the last value of its block.
        LoweringContext::SitePosition where { m_function.position, 0 };
        if (m_currentBlock && !m_currentBlock->values.isEmpty()) {
            auto& value = m_function.at(m_currentBlock->values[std::min<size_t>(m_currentIndexInBlock, m_currentBlock->values.size() - 1)]);
            where = { value.position, value.inlineFrame };
        }
        m_context.sitePositions.append(where);
        uint32_t bits = CallSiteIndex(BytecodeIndex(m_nextSite++)).bits();
        return m_function.inlineFrames.isEmpty() ? bits : PackedSite::pack(where.inlineFrame, bits);
    }
    LValue callEntry(Entry, LType, bool mayThrow, OperationEffects, const Vector<LValue, 8>& arguments);
    // It does not come back: m_out.unreachable() follows, as behind an operation that throws. The operand is taken as it is kept, in whatever register it is in.
    // Yields the call site.
    uint32_t throwAt(ThrowKind, std::optional<Operand>, uint32_t a, std::optional<uint32_t> b = std::nullopt);
    OperationEffects effectsOfBuiltinCall(const Value&);
    // Every call says what it may change through this, and every store that others can see through didEmitWrite.
    void applyEffects(B3::PatchpointValue*, OperationEffects);
    void didEmitWrite(Writes, bool writesShapeWords = false);
    template<auto operation, typename... Arguments> auto callOperation(Entry, OperationEffects, Arguments...);
    LValue callFunction(FunctionID, LType, const Vector<LValue, 8>& arguments);
    // Off the usual path, of a function that takes nothing and yields nothing: every register is kept, and a function without a frame stays without.
    void callFunctionFromColdBlock(FunctionID);
    uint32_t indexOfCallee(FunctionID);
    bool mayJumpTo(FunctionID, const Vector<LValue, 8>& arguments) const;
    void jumpToFunction(uint32_t indexOfCallee, const Vector<LValue, 8>& arguments);
    Operand jumpToContinuation(const Value&);
    Boxed callValue(Stub, const Vector<LValue, 8>& operands, OperationEffects = { });
    Boxed callValueWithArray(Stub, Boxed callee, Boxed thisValue, Boxed list);
    Boxed lowerGetNewTarget();
    Boxed callValueWithList(Boxed callee, Boxed thisValue, Word32 count, Pointer list);
    // The general entry of a function with a rest parameter, if that has the body of the function: it makes the array only where it is needed.
    FunctionID listEntryFor(FunctionID);
    Boxed callListEntry(FunctionID entry, LValue closure, Boxed thisValue, const Arguments&);
    // Of a NewArray: it is made for the rest parameter of a CallDirect that goes to the list entry, which takes its elements as they are.
    bool isPassedInMemory(ValueID);
    template<typename T, typename Functor> T lazilyInitialized(Stub, ptrdiff_t table, uint64_t indexAndMore, const Functor& fill);
    // By its number among Program::allocations: a cell with its header, and both words if the language sees it as an object. Its fields hold nothing yet.
    enum class FieldInit : uint8_t { ByCaller, FromTemplate };
    CellPointer allocate(uint32_t site) { return allocate(site, integer(site), FieldInit::ByCaller); }
    // `number`: the site itself, or the entry its discriminant picks.
    CellPointer allocate(uint32_t site, Integer number, FieldInit);
    // Yields the cell. The stub keeps whatever the site holds in registers, where it can.
    LValue callAllocationStub(Stub, const Vector<std::pair<LValue, GPRReg>, 4>& operands, OperationEffects, bool mayThrow);
    Integer allocationFor(const Value& newObject);
    std::optional<uint32_t> constantAllocation(const Value& newObject) const;
    bool layoutVariesByDiscriminant(const Allocation&) const;
    CellPointer stringConstant(StringID);
    // A leaf that stands for a cell of a lazily filled table, and that is lowered where it is asked for: see get() and findLeavesFetchedLate().
    bool isFetchedWhereNeeded(ValueID id) const { return m_isFetchedLate.get(id.index()); }
    void poll();
    std::pair<Word32, Pointer> argumentBuffer(const Arguments&);
    std::pair<CellPointer, Pointer> allocateArray(Word32 length, Word32 vectorLength, LBasicBlock slowPath);
    CellPointer newArray(const Arguments&);
    CellPointer newArrayFromBuffer(Word32 count, Pointer buffer);
    // Of a function that takes its arguments in memory.
    Word32 argumentCount();
    Pointer argumentBuffer();
    Word32 restLength(uint32_t numberOfArgumentsToSkip);
    CellPointer createRest(uint32_t numberOfArgumentsToSkip);
    Boxed lowerGetMyArgumentByVal(const Value&);
    Boxed lowerCallForwardVarargs(const Value&);
    Integer entryWordIfClosure(CellPointer);
    CellPointer createThis(CellPointer constructor, CellPointer newTarget);
    Boxed constructClosure(CellPointer constructor, CellPointer newTarget, const Arguments&);
    Boxed constructDerivedClosure(CellPointer constructor, Boxed newTarget, const Arguments&);
    Operand lowerConstructSuper(ValueID, const Value&);
    CellPointer lowerCreateThis(const Value&);
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
    Boxed callGenerally(Boxed callee, Boxed thisValue, const Arguments&, StringID nameOfMethod = { });
    Boxed callMethodGenerally(Boxed object, StringID, const Arguments&);
    Boxed callMethodThroughSlot(Boxed object, unsigned slot, const Arguments&);
    bool mayBeCalledInPlaceOfItsClosure(FunctionID, unsigned numberOfArguments) const;
    Operand callClosure(FunctionID, CellPointer closure, ValueID thisValue, const Arguments&, const Type& result, LBasicBlock otherwise);
    Operand lowerCallDirect(ValueID result, const Value&);
    // Where the values that a function with Function::resultTypes yields are: as arguments are passed.
    Vector<B3::ValueRep, 4> registersOfResults(const Function&) const;
    void returnResults(const Value&);
    Operand lowerResultAt(const Value&);
    // By call of such a function.
    UncheckedKeyHashMap<uint32_t, LValue, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_callsWithSeveralResults;
    LValue m_lastCallWithSeveralResults { nullptr };
    bool isGuardedCallee(ValueID);
    Operand lowerIndirectCall(ValueID callee, ValueID thisValue, const Arguments&, const Type& result, StringID nameOfMethod = { });
    Word32 lowerIsBuiltinFunction(const Value&);
    Boxed getCallOrApply(Boxed base, StringID, BuiltinID);
    std::optional<Operand> lowerCallOrApply(ValueID result, ValueID function, StringID, std::optional<Boxed> method, const Arguments&);
    Operand lowerApply(ValueID callee, ValueID thisValue, ValueID list, const Type& result, LanguageOperation = LanguageOperation::Apply);
    Operand lowerCall(ValueID result, const Value&);
    Operand lowerCallMethod(ValueID result, const Value&);
    Operand lowerConstruct(ValueID result, const Value&);
    // The arguments: the function, the object that was made for it, what it is passed.
    Operand lowerConstructWith(ValueID result, const Value&);
    FunctionID genericEntrypointFor(FunctionID);
    Operand captureOfClosure(CellPointer, const Function&, unsigned index);
    LValue loadRuntimeState32(ptrdiff_t offsetInInstance) { return m_out.load32(m_out.address(m_heaps.AOTTyped_runtimeState, m_instance, offsetInInstance)); }
    CellPointer closureConstant(FunctionID);
    CellPointer lowerNewClosure(const Value&);
    CellPointer lowerNewClosureOnce(const Value&);

    // The generic tier.
    LValue loadICData();
    std::optional<unsigned> allocateSite(StringID name, unsigned extra);
    void findICData();
    enum class SlotAddressIsFor : bool { UsualPath, ColdBlock };
    Pointer slotAddress(unsigned slot, SlotAddressIsFor = SlotAddressIsFor::UsualPath);
    LValue callICStub(Stub, LType, const Vector<std::pair<LValue, GPRReg>, 4>& operands, std::optional<std::pair<GPRReg, uint32_t>> immediate = std::nullopt, OperationEffects = { });
    // `isCalled`: it is the read of `o.m` in `o.m(..)`.
    Boxed emitGetById(Boxed base, StringID, bool isCalled = false);
    Boxed emitCachedGetById(Boxed base, StringID, bool isCalled = false);
    Boxed emitGetLength(Boxed base);
    void emitPutById(Boxed base, StringID, Boxed value);
    // `mayBeName`: the key is not expected to be a number. Then the site remembers one key and where it is in objects of one structure.
    Boxed emitGetByVal(Boxed base, Boxed key, bool mayBeName);
    void emitPutByVal(Boxed base, Boxed key, Boxed value, bool mayBeName);
    // False: the function has as many sites as it can have.
    bool emitDeleteByVal(Boxed base, Boxed key);
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
        // Of an instance of a class of the engine.
        bool hasOriginalStructure { false };
    };
    std::optional<Type> predictionOf(ValueID, uint16_t without = Primitive::UndefinedOrNull) const;
    Type expectedTypeOf(ValueID) const;
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
    bool isExpectedToBeNumber(ValueID element);
    Operand getArrayElement(CellPointer array, ValueID key, const Type&, LBasicBlock otherwise = nullptr, bool expectsNumbers = true);
    Word32 isBeyondBarrierThreshold(CellPointer owner);
    template<typename Functor> void storeBoxedArrayElement(CellPointer array, std::optional<Integer> index, Boxed stored, LBasicBlock otherwise, const Functor& afterwards);
    template<typename Functor> void storeArrayElement(CellPointer array, std::optional<Integer> index, ValueID stored, LBasicBlock otherwise, const Functor& afterwards);
    void putArrayElement(CellPointer array, ValueID key, ValueID stored, LBasicBlock otherwise = nullptr);
    Word32 tryStoreArrayElement(CellPointer array, Integer index, ValueID stored);
    Operand pushArrayElement(CellPointer array, ValueID receiver, ValueID pushed, const Type&);
    Operand pushArrayElements(CellPointer array, ValueID receiver, const Arguments& pushed, StringID name, const Type&);
    void ensureStillAliveHere(CellPointer owner, const char* file = __builtin_FILE(), unsigned line = __builtin_LINE());
    // Options::aotGuardToDropForTesting() names the place in the source of the compiler.
    static bool isDroppedForTesting(const char* file = __builtin_FILE(), unsigned line = __builtin_LINE());
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
        // Of a value that may be `undefined` or `null`: whether it is neither. If it is one of them, `string` is no cell.
        Word32 isString;
        Boxed value;
        // The type of the value admits nothing but cells, `undefined` and `null`: only then may the test be left to a fault. See markNilTest().
        bool isCellOrNil { false };
        // The value, where whoever made this had one: see openedString().
        ValueID id { };
    };
    struct Characters {
        // Where the first one is.
        Pointer data;
        // Of the StringImpl that has them: whether they are 8-bit. Whether it is an atom only shows for a string that is all of it.
        Word32 flags;
        Integer length;
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
    Characters charactersOf(const StringReceiver&, LBasicBlock ifRope = nullptr);
    Word32 codeUnitAt(const Characters&, Integer index);
    Word32 codePointAt(const Characters&, Integer index);
    CellPointer stringOfCodeUnit(Word32);
    CellPointer singleCharacterStringAt(const Characters&, Integer index, Word32* codeUnit = nullptr);
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
    // Of 8-bit characters, as many on either side.
    Word32 areBytesEqual(Pointer, Pointer, Integer length);
    Word32 hasTextAt(const StringReceiver&, Integer place, ValueID constant, std::span<const uint8_t> text);
    Word32 isStringWithText(ValueID, ValueID constant, std::span<const uint8_t> text);
    Word32 areStringsEqual(ValueID, ValueID);
    Boxed mapKey(ValueID);
    Word32 hashOfMapKey(Boxed);
    Pointer findInMapOrSet(CellPointer table, bool isMap, Boxed key, Word32 hash, LBasicBlock absent);
    Integer sizeOfMapOrSet(CellPointer, bool isMap);
    Operand lowerMapOrSetBuiltin(InlineBuiltin, ValueID result, const Value&, Writes inPlace);

    // The table.
    CellPointer builtinCell(unsigned);
    Word32 hasOriginalStructure(CellPointer, const EngineClass&);
    Boxed callBuiltinFunction(const Value&, OperationEffects, std::optional<Boxed> calledOn = std::nullopt);
    Operand lowerGenericFallback(const Value&, CellPointer receiver, const Type&);
    Boxed lowerMethodRead(const Value&);
    Operand lowerCallBuiltin(ValueID result, const Value&);
    Operand lowerResolvedBuiltin(ValueID result, const Value&);
    Operand lowerIndexIntrinsic(InlineBuiltin, const Value&, const Type&);
    // What a built-in yields, as the type of the value that stands for the call.
    Operand yieldNumber(Number, const Type&);
    Operand yieldBoolean(Word32, const Type&);
    Operand yieldValue(ValueID, const Type&);
    // In AOTTypedBuiltins<Family>.cpp. `inPlace`: what writesOfInline<Family>() says.
#define AOT_TYPED_DECLARE_LOWER_FAMILY(family) std::optional<Operand> lowerInline ## family(InlineBuiltin, ValueID result, const Value&, Writes inPlace);
    FOR_EACH_AOT_TYPED_BUILTIN_FAMILY(AOT_TYPED_DECLARE_LOWER_FAMILY)
#undef AOT_TYPED_DECLARE_LOWER_FAMILY
#include "AOTTypedBuiltinMembers.inc"
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
    std::optional<ExpectedBuiltin> expectedBuiltin(BuiltinKind, ValueID receiver, StringID, const Arguments&) const;
    bool chooseArgumentsToExpect(ExpectedBuiltin&) const;
    std::optional<Operand> lowerCallOfBuiltinFunction(ValueID result, ValueID callee, ValueID thisValue, const Arguments&);
    template<typename Generally> Operand lowerExpectedBuiltin(ValueID result, const ExpectedBuiltin&, std::optional<Boxed> callee, const Generally&);

    const Program& m_program;
    const Function& m_function;
    FunctionID m_id;
    bool m_isEntry;
    LoweringContext& m_context;
    const Builtins& m_builtins;
    Vector<Operand> m_values;
    Vector<Assumption, 4> m_assumptions;
    struct FusedCasts {
        size_t indexOfRead;
        unsigned count;
        // That of the last cast, as an object of its own: ensure() knows by its address that it is asked for what the read yields.
        Type type;
        bool areBeingCarriedOut { false };
        // The operation that is carried out in the arms behind the casts: see lowerFusedUse(). Then `type` is its type.
        std::optional<size_t> indexOfUse { };
        // Casts behind that operation which only test, for a type without objects: the operation is lowered for the type of the last.
        unsigned countBehindUse { 0 };
    };
    std::optional<FusedCasts> m_fusedCasts;
    Vector<uint32_t> m_numberOfUses;
    UncheckedKeyHashMap<uint32_t, PinnedRead, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_pinnedReads;
    Vector<LBasicBlock> m_blocks;
    Vector<Vector<LValue, 4>> m_phis;
    // By block parameter: whether it is truthy, as every edge says of what it passes. See findTruthinessPhis().
    BitVector m_hasTruthinessPhi;
    BitVector m_isFetchedLate;
    BitVector m_isCarriedOutByBranch;
    UncheckedKeyHashMap<uint32_t, LValue, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_truthinessPhis;
    // By block parameter that is passed in parts: the phi of the cell. That of the word is in m_phis. See findParametersJoinedInParts().
    BitVector m_isJoinedInParts;
    UncheckedKeyHashMap<uint32_t, LValue, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_cellPhis;
    // The answer of the test that is being lowered is kept, not branched on: a comparison yields it in fewer instructions than isNoCell().
    bool m_answerIsValue { false };
    // The cast and the value behind it are being lowered by themselves, on the side where the test of the latter has failed.
    bool m_isCarryingOutSunkCoercion { false };
    // For an integer that was made from a double: that double.
    UncheckedKeyHashMap<LValue, LValue> m_asDouble;
    // For what is a cell from where it is defined, so that a load from it cannot trap wherever it is moved to: its type.
    UncheckedKeyHashMap<LValue, TypeID> m_typeOfCell;
    // What was defined as a boxed value and is a cell only behind the test that showed it.
    UncheckedKeyHashSet<LValue> m_takenForCells;
    struct Decoded {
        LValue index;
        LValue cell;
        LValue isNoCell;
        // The word of the slot, and whether `index` has `undefined` where that is zero.
        LValue loaded;
        bool absentReadsUndefined;
    };
    // For a boxed value that boxDecoded() made. The index is only ever compared with constants: it keeps nothing alive. Also for what readField() has made of a 64-bit slot: there the index
    // is the boxed value and the cell is what was loaded.
    UncheckedKeyHashMap<LValue, Decoded> m_decoded;
    // For a cell that load() has decoded: the word of the slot. It keeps nothing alive: it is compared, no more: see isTheClosure().
    UncheckedKeyHashMap<LValue, LValue> m_indexOfCell;
    StringID m_length;
    LValue m_icData { nullptr };
    // See slotAddress().
    B3::Variable* m_icDataAsOfLastHint { nullptr };
    LBasicBlock m_blockOfICData { nullptr };
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
    // See isGuardedCallee().
    bool m_didFindGuardedCallees { false };
    BitVector m_guardedCallees;
    BitVector m_blocksThatEvaluateModules;
    UncheckedKeyHashMap<uint32_t, FunctionID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_listEntries;
    // See isPassedInMemory(). By CallDirect: the list entry it goes to.
    bool m_didFindRestArraysPassedInMemory { false };
    BitVector m_restArraysPassedInMemory;
    UncheckedKeyHashMap<uint32_t, FunctionID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_listEntryOfCall;
    // What is being stored to was made by this run of the function: to whoever calls it, such a store is none.
    bool m_isStoringToLocalAllocation { false };
    Vector<uint8_t> m_numberOfComparisonsWithIntegers;
    struct IntegerOfNumber {
        LValue number;
        LValue integer;
        // The integer is all there is to the number: none is below this, and INT64_MIN stands for what is not a number. Without it the integer says no more than which constant the number is equal to.
        std::optional<int64_t> least { };
    };
    UncheckedKeyHashMap<uint32_t, IntegerOfNumber, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_integersOfNumbers;
    bool m_isLoweringInitialStoresOneByOne { false };
    // While a value is fitted that something is made of where it does not fit (`as`, `!`): an object fits a view only if it is of a member,
    // also where the view admits foreign objects.
    bool m_foreignObjectsDoNotFit { false };
    // While the inline test of an As or Flow cast is emitted: Layout::castTest instead of Layout::test, so that a member the cast
    // transitions goes to the slow path.
    bool m_useCastTest { false };
    // validateFact() asks what holds of a value, which for an array is not what it takes to be taken for one: see EngineClass::type.
    bool m_isValidatingFact { false };
    // Where the value that is being lowered is.
    const Block* m_currentBlock { nullptr };
    // By object: where collectorMayHaveRunSinceBirth() was last asked from, and what it said.
    struct WalkedBackFrom {
        const Block* block { nullptr };
        size_t index { 0 };
        bool collectorMayHaveRun { false };
    };
    mutable UncheckedKeyHashMap<uint32_t, WalkedBackFrom, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_walkedBackFrom;
    // By such a leaf: where it was fetched last, and what.
    UncheckedKeyHashMap<uint32_t, std::pair<LBasicBlock, Operand>, WTF::IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_fetchedLeaves;
    // See objectLiteralAt().
    const Block* m_blockOfObjectLiterals { nullptr };
    Vector<ObjectLiteral, 1> m_objectLiterals;
    // See namedStoresAt().
    const Block* m_blockOfNamedStores { nullptr };
    Vector<NamedStores, 1> m_namedStores;
    // See accessorGroupAt().
    const Block* m_blockOfAccessorGroups { nullptr };
    Vector<AccessorGroup, 1> m_accessorGroups;
    // See propertyRunAt().
    const Block* m_blockOfPropertyRuns { nullptr };
    Vector<PropertyRun, 1> m_propertyRuns;
    BlockID m_currentBlockID;
    size_t m_currentIndexInBlock { 0 };
    // What is appended here is run whenever the value that is being lowered is.
    LBasicBlock m_unconditionalBlock { nullptr };
    Vector<unsigned> m_dominatorTreeEnter;
    Vector<unsigned> m_dominatorTreeLeave;
    struct Loop {
        BlockID header;
        BitVector blocks;
        bool hasHandlerInside { false };
    };
    static constexpr unsigned noLoop = UINT_MAX;
    Vector<Loop> m_loops;
    Vector<unsigned> m_innermostLoop;
    Vector<BlockID> m_definingBlock;
    // By value: the blocks in which it has passed nonNil().
    UncheckedKeyHashMap<uint32_t, Vector<BlockID, 1>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_passedNilTestIn;
    // By block that only the other edge of such a test leads to: the value that has been found nil there. See lowerCall().
    UncheckedKeyHashMap<uint32_t, ValueID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_foundNilIn;
    mutable UncheckedKeyHashMap<uint32_t, std::unique_ptr<Type>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_typesWithoutNil;
    // Of a general entry that takes a list.
    Word32 m_argumentCount;
    Pointer m_argumentBuffer;
    // Where there are handlers: these two are kept for them as what is live into them is. See computeCatchEntrypoints().
    B3::Variable* m_argumentCountVariable { nullptr };
    B3::Variable* m_argumentBufferVariable { nullptr };
    unsigned m_firstArgumentWord { 0 };
    bool m_isInLoop { false };
    // In the general arm of a site that has tested its operands for int32 in line.
    bool m_hasFailedInt32Test { false };
    uint32_t m_nextSite { 1 };
    ASCIILiteral m_whyFailed;
    uint8_t m_genericEntrypointParameterCount { 0 };
};

// Where the arms of a test come together. A reference that an arm has in parts is passed in parts by all of them, so that nobody puts it together for a test behind the join that takes it
// apart again: see Lowering::Parts. An arm is left open until finish(): only then is it known how the values are passed.
class Lowering::OperandJoin {
public:
    OperandJoin(Lowering& lowering, Joined joined)
        : m_lowering(lowering)
        , m_joined(joined)
        , m_continuation(lowering.m_out.newBlock())
    {
    }

    // The block is done with.
    void add(Operand operand)
    {
        RELEASE_ASSERT(operand.rep == m_joined.rep);
        m_arms.append({ m_lowering.m_out.m_block, operand });
    }

    Operand finish();

private:
    struct Arm {
        LBasicBlock block;
        Operand operand;
    };

    Lowering& m_lowering;
    Joined m_joined;
    LBasicBlock m_continuation;
    Vector<Arm, 4> m_arms;
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
Operand Lowering::eitherOperand(Word32 condition, Joined joined, const Fast& fast, const Slow& slow)
{
    LBasicBlock yes = m_out.newBlock();
    LBasicBlock no = newColdBlock();
    OperandJoin join(*this, joined);
    m_out.branch(condition.value(), usually(yes), rarely(no));
    m_out.appendTo(yes);
    join.add(fast());
    m_out.appendTo(no);
    join.add(slow());
    return join.finish();
}

template<typename Passes, typename Fails>
Operand Lowering::byShape(CellPointer cell, const ShapeTest& test, Joined joined, const Passes& ifItPasses, const Fails& ifItFails, bool mostlyFails)
{
    if (test.passesNothing())
        return ifItFails();
    LBasicBlock passing = mostlyFails ? newUnlikelyBlock() : m_out.newBlock();
    LBasicBlock failing = mostlyFails ? m_out.newBlock() : newUnlikelyBlock();
    OperandJoin join(*this, joined);
    m_out.branch(passes(shapeWordOf(cell), test, failing).value(), unsure(passing), unsure(failing));
    m_out.appendTo(passing);
    join.add(ifItPasses());
    m_out.appendTo(failing);
    join.add(ifItFails());
    return join.finish();
}

template<typename Has, typename Lacks>
Operand Lowering::throughView(CellPointer cell, ValueID object, LayoutID view, const Field& field, Joined joined, const Has& has, const Lacks& lacks)
{
    // The members that have the field are members.
    if (field.isLackedBySomeMember) {
        // Placement has seen this access: see Field::slot. Otherwise a member has the property in a field this one is not tied to, and
        // what is done for an object that lacks the field would go past it.
        RELEASE_ASSERT(!field.membersWithIt.passesNothing() || !hasMemberWithField(view, field.name));
        return byShape(cell, field.membersWithIt, joined, has, lacks, mostMembersLack(view, field));
    }
    return ifOfMember(cell, object, view, joined, has, lacks);
}

template<typename InPlace, typename Generally>
Operand Lowering::ifOfMember(CellPointer cell, ValueID object, LayoutID view, Joined joined, const InPlace& inPlace, const Generally& generally)
{
    if (!m_program.at(view).admitsForeignObjects || isAllocatedAsMemberOf(object, view))
        return inPlace();
    return byShape(cell, m_program.at(view).test, joined, inPlace, generally);
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
        if (!expectation.hasOriginalStructure && typeOf(expectation.value).isWithin(*expectation.type, m_program))
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
    // The way for doubles is the stub's, but in a loop: in line it is ten times the code of the way for int32, at every site.
    if (mayAllBeInt32 && !m_isInLoop)
        mayBeOtherNumbers = false;
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
    SetForScope hasFailedInt32Test(m_hasFailedInt32Test, mayAllBeInt32);
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
    // All of them boxed as int32: the form for integers. Else as numbers.
    auto& int32Type = m_program.at(m_program.types.int32);
    BuiltinID formForIntegers;
    if (auto& builtin = m_builtins.at(expected.call.builtin()); !receiver && builtin.kind == BuiltinKind::StaticFunction && !expected.arguments.isEmpty()) {
        Vector<Type, 4> integers;
        integers.fill(int32Type, expected.arguments.size());
        formForIntegers = applicableEntry(m_builtins.findStatic(BuiltinKind::StaticFunction, builtin.holderName, builtin.nameName), integers.span());
    }
    bool mayAllBeInt32 = formForIntegers && m_builtins.at(formForIntegers).parameters.begin()->conversion == ArgumentConversion::RequiresInteger;
    bool isAnyBoxed = false;
    for (unsigned i = 0; mayAllBeInt32 && i < expected.arguments.size(); ++i) {
        auto& known = typeOf(expected.call.arguments[i]);
        bool isBoxed = get(expected.call.arguments[i]).rep == Rep::JSValue && expected.arguments[i].isNumber() && !expected.arguments[i].isInteger();
        mayAllBeInt32 = isBoxed || (known.isInteger() && known.integers.isWithin(Range::int32()));
        isAnyBoxed |= isBoxed;
    }
    if (mayAllBeInt32 && isAnyBoxed) {
        Rep rep = repOf(typeOf(result));
        Value call = expected.call;
        call.type = m_function.at(result).type;
        LBasicBlock numbers = m_out.newBlock();
        Join join(m_out, rep, b3TypeOf(rep));
        size_t numberOfAssumptions = m_assumptions.size();
        for (ValueID argument : expected.call.arguments) {
            if (get(argument).rep == Rep::JSValue)
                m_assumptions.append(Assumption { argument, expectOrElse(Expectation { argument, &int32Type }, numbers), &int32Type });
        }
        join.add(*lowerInline(which, result, call));
        m_assumptions.shrink(numberOfAssumptions);
        m_out.appendTo(numbers);
        join.add(ifAsExpected(expectations, rep, [&](LBasicBlock) {
            return *lowerInline(which, result, call);
        }, generally));
        return join.finish();
    }
    return ifAsExpected(expectations, repOf(typeOf(result)), [&](LBasicBlock otherwise) {
        if (receiver && callee)
            orElse(equal(*callee, box(builtinCell(m_builtins.cellOf(expected.call.builtin())))), otherwise);
        // It takes the place of that value.
        Value call = expected.call;
        call.type = m_function.at(result).type;
        return *lowerInline(which, result, call);
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

// An entry of a table of the instance, which is filled the first time it is asked for. The index is the low half of the word, all of which
// goes to what fills the entry.
template<typename T, typename Functor>
T Lowering::lazilyInitialized(Stub stub, ptrdiff_t table, uint64_t indexAndMore, const Functor& fill)
{
    uint32_t index = static_cast<uint32_t>(indexAndMore);
    // In a loop the load and the test are in place.
    if (!m_isInLoop)
        return T(callAllocationStub(stub, { { m_out.constInt64(indexAndMore), stubTemporaryGPRs[0] } }, { Writes::RuntimeState, false }, false));
    LBasicBlock slowPath = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    // What fills it writes the state of the run time: a load that was moved out of a loop would find it empty every time around.
    LValue value = table == Instance::offsetOfTypedClosures() ? closureIfMade(index) : m_out.load64(m_out.address(m_heaps.AOTTyped_runtimeState, fixedPointer(table), static_cast<ptrdiff_t>(index) * sizeof(void*)));
    ValueFromBlock fast = m_out.anchor(value);
    m_out.branch(table == Instance::offsetOfTypedClosures() ? m_out.notEqual(value, noClosure()) : m_out.notZero64(value), usually(continuation), rarely(slowPath));
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
