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
#include <wtf/Box.h>
#include <wtf/SetForScope.h>

namespace JSC { namespace AOT { namespace Typed {

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

using Word32 = Lowered<Rep::Int32>;
using Integer = Lowered<Rep::Int64>;
using Number = Lowered<Rep::Double>;
using OptionalInteger = Lowered<Rep::OptionalInt64>;
using OptionalNumber = Lowered<Rep::OptionalDouble>;
using CellPointer = Lowered<Rep::Cell>;
using Boxed = Lowered<Rep::JSValue>;
using LiteralStringWord = Lowered<Rep::LiteralString>;

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

class Join {
public:
    Join(Output& out, Rep rep, LType type)
        : m_out(out)
        , m_rep(rep)
        , m_type(type)
        , m_continuation(out.newBlock())
    {
    }

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

template<typename T> struct OperationParameterType;
template<> struct OperationParameterType<EncodedJSValue> { using Type = Boxed; };
template<> struct OperationParameterType<JSCell*> { using Type = CellPointer; };
template<> struct OperationParameterType<uint32_t> { using Type = Word32; };
template<> struct OperationParameterType<int32_t> { using Type = Word32; };
template<> struct OperationParameterType<uint64_t> { using Type = Integer; };
template<> struct OperationParameterType<double> { using Type = Number; };
template<> struct OperationParameterType<const EncodedJSValue*> { using Type = Pointer; };
template<> struct OperationParameterType<EncodedJSValue*> { using Type = Pointer; };
template<> struct OperationParameterType<AOT::Slot*> { using Type = Pointer; };

template<typename T> struct OperationResultType;
template<> struct OperationResultType<void> { using Type = void; };
template<> struct OperationResultType<EncodedJSValue> { using Type = Boxed; };
template<> struct OperationResultType<JSCell*> { using Type = CellPointer; };
template<> struct OperationResultType<size_t> { using Type = Word32; };
template<> struct OperationResultType<double> { using Type = Number; };
template<> struct OperationResultType<void*> { using Type = Pointer; };

template<typename T>
struct UnwrappedOperationResult {
    using Type = T;
    static constexpr bool carriesException = false;
};
template<typename T>
struct UnwrappedOperationResult<ExceptionOperationResult<T>> {
    using Type = T;
    static constexpr bool carriesException = true;
};

template<typename Function> struct OperationSignature;
template<typename Result, typename... Parameters>
struct OperationSignature<Result(Instance*, Parameters...)> {
    using ResultType = typename OperationResultType<typename UnwrappedOperationResult<Result>::Type>::Type;
    static constexpr bool mayThrow = UnwrappedOperationResult<Result>::carriesException || std::is_same_v<Result, double>;
    static Vector<LValue, 8> list(typename OperationParameterType<Parameters>::Type... arguments) { return { arguments.value()... }; }
};
template<typename Result, typename... Parameters>
struct OperationSignature<Result(Parameters...)> {
    using ResultType = typename OperationResultType<Result>::Type;
    static constexpr bool mayThrow = false;
    static Vector<LValue, 8> list(typename OperationParameterType<Parameters>::Type... arguments) { return { arguments.value()... }; }
};
#if OS(WINDOWS) && CPU(X86_64)
template<typename Result, typename... Parameters>
struct OperationSignature<Result SYSV_ABI(Parameters...)> : OperationSignature<Result(Parameters...)> { };
#endif

struct LoweringContext {
    StubCalls stubCalls;
    struct DataInCodeLabel {
        CCallHelpers::Label start;
        unsigned numberOfWords;
    };
    Vector<DataInCodeLabel> dataInCode;
    void recordDataInCode(CCallHelpers& jit, size_t numberOfWords)
    {
        dataInCode.append({ jit.labelIgnoringWatchpoints(), static_cast<unsigned>(numberOfWords) });
    }
    RefPtr<B3::StackmapGenerator> numberBoxingGenerator;
    RefPtr<B3::StackmapGenerator> slotBlockFetchGenerator;
    unsigned numberOfSlotBlockFetches { 0 };
    Vector<ImageKey> knownCallees;
    UncheckedKeyHashMap<uint32_t, uint32_t, WTF::IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> calleeIndices;
    AddressLikeIntegers wideIntegerConstants;
    IndexReferences indexReferences;
    unsigned numberOfSiteCacheWords { 0 };
    Vector<std::pair<CCallHelpers::Label, CCallHelpers::Label>> rangesKeptInline;
    Vector<Site> sites;
    Vector<StringID> identifiers;
    Vector<uint32_t> plans;
    Vector<uint32_t> savedFramePlanOffsets;
    Vector<std::pair<uint32_t, uint32_t>> slotPlanOffsets;
    struct SitePosition {
        uint32_t position;
        uint32_t inlineFrame;
        bool constructs { false };
        FunctionID parameterCheckCallee { };
    };
    Vector<SitePosition> sitePositions;
    Vector<SpreadSite> itemSites;
    Vector<uint32_t> inlineCallSites;
    Vector<uint32_t, 2> framesDroppedByTailCalls;
    uint32_t numberOfSitesWithoutCode { 0 };
    unsigned numberOfInlineEmptyArrayLiterals { 0 };
    unsigned numberOfStubEmptyArrayLiterals { 0 };
    bool unwindsToLandingPads { false };
    Vector<Box<CCallHelpers::Label>> landingPads;
    UncheckedKeyHashMap<CCallHelpers::Label*, uint32_t> landingPadsByHandler;
    Vector<std::pair<uint32_t, uint32_t>> landingPadsForCallSites;
    struct NullishTest {
        ThrowKind kind;
        uint32_t a;
        std::optional<uint32_t> b;
        uint32_t callSite;
    };
    Vector<NullishTest> nullishTests;
    RefPtr<B3::StackmapGenerator> nullishTestMarker;
    RefPtr<B3::StackmapGenerator> emptyGenerator;
    UncheckedKeyHashMap<B3::Value*, unsigned> accessesThroughPointer;
    bool keepsCellsAliveForWords { false };
    struct LatePathCallAccess {
        unsigned child;
        uint32_t nullishTest { std::numeric_limits<uint32_t>::max() };
    };
    UncheckedKeyHashMap<RefPtr<B3::StackmapGenerator>, Box<LatePathCallAccess>> latePathCallAccesses;
    struct FaultingAccess {
        CCallHelpers::Label instruction;
        GPRReg base;
        uint32_t nullishTest;
    };
    Vector<FaultingAccess> faultingAccesses;
    struct ThrowSiteTrap {
        CCallHelpers::Label instruction;
        uint32_t callSite;
    };
    Vector<ThrowSiteTrap> throwSiteTraps;
    Vector<ThrowSiteTrap> loopPollTraps;
    struct CallThrowSite {
        CCallHelpers::Label call;
        Vector<uint32_t, 3> words;
    };
    Vector<CallThrowSite> callThrowSites;
    UncheckedKeyHashMap<RefPtr<B3::StackmapGenerator>, uint32_t> loopPollSites;
    bool emitsCalls { false };
    BitVector blocksWithCalls;
    BitVector unreachableBlocks;
    bool readsCallerFrame { false };
    bool savesFrameOnSuspend { false };
    bool needsStackCheckForTailCalls { false };
    bool tailCallsNeedFrame { false };
    bool dropsCollapsibleCallerFrames { false };
    bool hasColdSites { false };
    bool callsPreserveLiveRegisters { false };
    bool createsFrameRecord { false };
    unsigned stackUsedAtCalls { 0 };
    StringBuilder literalsForMap;
    StringBuilder branchHintsForMap;

    static bool coldSitesForceFrame()
    {
        if (hasFramelessFunctionStubs)
            return false;
        return !isX86_64();
    }

    static bool treatsColdSitesAsCallsForSplitting()
    {
        return isX86_64();
    }

    bool hasNoFrame(B3::Air::Code& code) const
    {
        return !code.frameSize() && !code.calleeSaveRegisterAtOffsetList().registerCount() && !emitsCalls && !readsCallerFrame && !createsFrameRecord && !needsStackCheckForTailCalls && !tailCallsNeedFrame;
    }

    void emitCalleeSaveRestoreKeepingFrame(CCallHelpers& jit, B3::Air::Code& code) const
    {
        RELEASE_ASSERT(!hasNoFrame(code) && !code.placesSpillsAboveFrameRecord());
        jit.emitRestore(code.calleeSaveRegisterAtOffsetList());
    }

    void emitFrameTeardown(CCallHelpers& jit, B3::Air::Code& code) const
    {
        if (hasNoFrame(code))
            return;
#if CPU(ARM64)
        if (code.placesSpillsAboveFrameRecord()) {
            popCalleeSaves(jit, code.calleeSaveRegisterAtOffsetList());
            jit.m_assembler.ldp<64>(GPRInfo::callFrameRegister, CCallHelpers::linkRegister, CCallHelpers::stackPointerRegister, PairPostIndex(static_cast<int>(code.bytesAboveFrameRecord() + sizeof(CallerFrameAndPC))));
            return;
        }
#endif
#if CPU(X86_64)
        if (code.placesCalleeSavesAtFrameBottom()) {
            RegisterAtOffsetList saves = code.calleeSaveRegisterAtOffsetList();
            for (size_t i = 0; i < saves.registerCount(); ++i)
                jit.pop(saves.at(i).reg().gpr());
            if (code.frameSize() == saves.registerCount() * sizeof(CPURegister))
                jit.emitFunctionEpilogueWithEmptyFrame();
            else
                jit.m_assembler.buffer().putByte(static_cast<int8_t>(0xc9));
            return;
        }
#endif
        jit.emitRestore(code.calleeSaveRegisterAtOffsetList());
        if (!code.frameSize()) {
            jit.emitFunctionEpilogueWithEmptyFrame();
            return;
        }
#if CPU(X86_64)
        jit.m_assembler.buffer().putByte(static_cast<int8_t>(0xc9));
#else
        jit.emitFunctionEpilogue();
#endif
    }
};

Vector<uint32_t, 3> throwSiteWords(ThrowKind, Rep, unsigned registerNumber, uint32_t a, std::optional<uint32_t> b);
static constexpr int64_t maxFaultingAccessOffset = 16000;
void useFaultsForNullishTests(B3::Procedure&, LoweringContext&);
#if AOT_CHECKS_ASAN_SHADOW_IN_COMPILED_CODE
void checkShadowBeforeAccesses(B3::Procedure&);
#endif
void foldDecodingsIntoAccesses(B3::Procedure&, LoweringContext&);
void branchOverUndefinedForAbsentCells(B3::Procedure&);
void splitBranchConditions(B3::Procedure&);
void foldNumberBoxings(B3::Procedure&, LoweringContext&);
void loadDoubleConstantsFromInstance(B3::Procedure&);
void narrowSignExtendedComparisons(B3::Procedure&);
void narrowZeroExtendedComparisons(B3::Procedure&);
void storeAdjacentConstantsTogether(B3::Procedure&);
void useMemoryOperandsAtLoopPolls(B3::Procedure&, LoweringContext&);
void placeSlotBlockFetches(B3::Procedure&, LoweringContext&);

struct OperationEffects {
    Writes writes { Writes::Heap };
    bool exitsSideways { true };
    bool writesShapeWords { writes == Writes::Heap };
    bool writesOnlyOrderWords { false };
    bool writesOnlyTypedArrayProperties { false };
    bool convertsArrayStorage { writes == Writes::Heap };
};

class Lowering final : public Emitter {
public:
    Lowering(B3::Procedure&, const Program&, FunctionID, const Function&, LoweringContext&);

    bool run();
    ASCIILiteral failureReason() const { return m_failureReason; }
    bool usesUndefinedValue() const { return m_usesUndefinedValue; }
    uint8_t genericEntrypointParameterCount() const { return m_genericEntrypointParameterCount; }

private:
    void fail(ASCIILiteral why)
    {
        if (m_failureReason.isNull())
            m_failureReason = why;
    }
    Operand failWith(ASCIILiteral why)
    {
        fail(why);
        return undefined();
    }
    const Type& typeOf(ValueID id) const
    {
        for (size_t i = m_assumptions.size(); i--;) {
            if (m_assumptions[i].value == id)
                return *m_assumptions[i].type;
        }
        auto& type = m_program.at(m_function.at(id).type);
        return type.mayBeUndefinedOrNull() && !type.isUndefinedOrNull() && hasPassedNullishTest(id) ? withoutNullish(m_function.at(id).type) : type;
    }
    bool hasPassedNullishTest(ValueID) const;
    void didPassNullishTest(ValueID, BlockID);
    const Type& withoutNullish(TypeID) const;
    Operand get(ValueID);
    bool encodesUndefinedAsZero(ValueID) const;
    bool isKnownDefinedHere(ValueID) const;
    Operand decodeUndefined(ValueID, Operand);
    ValueID readProvenDefinedWhenFalse(const Terminator&) const;
    void define(ValueID, Operand);
    static LType b3TypeOf(Rep);
    LBasicBlock newColdBlock();
    LBasicBlock newUnlikelyBlock();
    std::span<const Target> successorsOf(const Terminator&) const;
    Vector<BlockID> reversePostorder() const;
    struct PredecessorLists {
        std::span<const unsigned> operator[](unsigned block) const { return blocks.span().subspan(first[block], first[block + 1] - first[block]); }

        Vector<unsigned, 32> placeInOrder;
        Vector<unsigned, 33> first;
        Vector<unsigned, 48> blocks;
    };
    PredecessorLists computePredecessors(const Vector<BlockID>& order) const;
    void computeLoopBlocks(const Vector<BlockID>& order, const PredecessorLists&);
    void computeDominators(const Vector<BlockID>& order, const PredecessorLists&);
    struct Loop;
    void computeLoops(const Vector<BlockID>& order, const PredecessorLists&);
    void applyBranchHints(const Vector<BlockID>& order);
    bool verifyDefinitions(const Vector<BlockID>& order) const;
    bool isLoopInvariant(const Loop&, ValueID) const;
    bool isGuaranteedToExecute(const Loop&) const;
    bool willHoistComputationOn(ValueID) const;
    bool dominates(BlockID a, BlockID b) const { return m_dominatorTreeEnter[a.index()] <= m_dominatorTreeEnter[b.index()] && m_dominatorTreeLeave[b.index()] <= m_dominatorTreeLeave[a.index()]; }
    bool lowerEntryParameters();

    enum class RegistersAtLandingPad : uint8_t { Clobbered, PreservedFromSite };
    void unwindToLandingPad(B3::PatchpointValue*, CallSite, RegistersAtLandingPad = RegistersAtLandingPad::Clobbered, bool mayReturn = true);
    void branchToLandingPad(B3::PatchpointValue*, std::optional<GPRReg> exception);
    Boxed catchException(BlockID handler);
    ValueID exceptionCaughtIn(BlockID handler) const;
    bool ignoresCaughtValue(BlockID handler);
    void didLowerOperationWithHandler();
    LBasicBlock landingPadFor(const Target&);
    void findHandlersEnteredByBranch(const Vector<BlockID>& order);
    LBasicBlock handlerEnteredByBranch() const;
    void convertPlainRowInPlace(ValueID row, TypeID);
    void lowerBlock(BlockID);
    void lowerTerminator(const Terminator&);
    std::optional<unsigned> nullishSuccessorOf(const Terminator&) const;
    std::optional<bool> likelyAnswer() const;
    std::optional<unsigned> differingSuccessorOf(const Terminator&) const;
    std::optional<std::pair<ValueID, unsigned>> nullishTestOf(const Terminator&) const;
    LBasicBlock edgeTo(const Target&, bool isBranch);
    void countBranchTo(const Target&);
    Operand lower(ValueID, const Value&);
    Operand lowerLanguageOperation(ValueID result, const Value&);
    CellPointer lowerNewPromise();
    Boxed lowerResolvePromise(const Value&);
    bool isPromiseOfUnsuspendedFunction(ValueID) const;
    void findBlocksAroundAwaits();
    bool awaitsBefore(const Block&, size_t end) const;
    bool isReachedWithoutAwaiting();
    BitVector m_blocksReachedWithoutAwaiting;
    BitVector m_blocksReachedAfterAwait;
    bool isNotThenableWithoutExtraProperties(const Type&) const;

    template<typename Functor> void require(Word32 condition, const Functor& otherwise);
    template<typename T, typename Fast, typename Slow> T either(Word32 condition, const Fast&, const Slow&);
    struct JoinRep {
        JoinRep(Rep rep, bool isReference = false)
            : rep(rep)
            , isReference(isReference)
        {
        }

        Rep rep;
        bool isReference;
    };
    JoinRep joinRepFor(const Type&) const;
    class OperandJoin;
    template<typename Fast, typename Slow> Operand eitherOperand(Word32 condition, JoinRep, const Fast&, const Slow&);
    template<typename Fast, typename Slow> Pointer eitherPointer(Word32 condition, const Fast&, const Slow&);
    template<typename Usual, typename Rare> void ifElse(Word32 condition, const Usual&, const Rare&);
    template<typename Functor> void inColdBlock(LBasicBlock, const Functor&);

    Word32 word32(int32_t value) { return Word32(m_out.constInt32(value)); }
    Word32 boolean(bool value) { return word32(value); }
    Integer integer(int64_t);
    Number number(double value) { return Number(m_out.constDouble(value)); }
    Boxed boxedConstant(JSValue value) { return Boxed(m_out.constInt64(JSValue::encode(value))); }
    Boxed undefined() { return boxedConstant(jsUndefined()); }
    template<typename Result, typename T> Result unary(LValue (Output::*operation)(LValue), T a) { return Result((m_out.*operation)(a.value())); }
    template<typename Result, typename T> Result binary(LValue (Output::*operation)(LValue, LValue), T a, T b) { return Result((m_out.*operation)(a.value(), b.value())); }

    Integer add(Integer a, Integer b) { return binary<Integer>(&Output::add, a, b); }
    Integer sub(Integer a, Integer b) { return binary<Integer>(&Output::sub, a, b); }
    Integer mul(Integer a, Integer b) { return binary<Integer>(&Output::mul, a, b); }
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
    Word32 equalOrUnordered(Number a, Number b) { return binary<Word32>(&Output::doubleEqualOrUnordered, a, b); }
    Word32 isNeitherZeroNorNaN(Number a) { return isX86_64() ? binary<Word32>(&Output::doubleNotEqualAndOrdered, a, number(0)) : greaterThan(abs(a), number(0)); }
    Word32 lessThan(Number a, Number b) { return binary<Word32>(&Output::doubleLessThan, a, b); }
    Word32 lessThanOrEqual(Number a, Number b) { return binary<Word32>(&Output::doubleLessThanOrEqual, a, b); }
    Word32 greaterThan(Number a, Number b) { return binary<Word32>(&Output::doubleGreaterThan, a, b); }
    Word32 greaterThanOrEqual(Number a, Number b) { return binary<Word32>(&Output::doubleGreaterThanOrEqual, a, b); }
    Number toNumber(Word32 a) { return unary<Number>(&Output::intToDouble, a); }
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
    Word32 hasNoBits(Word32 a, uint32_t mask) { return binary<Word32>(&Output::testIsZero32, a, word32(mask)); }
    Word32 isContiguousOrInt32Shape(Word32 shape)
    {
        static_assert(ContiguousShape == 2 * Int32Shape && hasOneBitSet(static_cast<unsigned>(Int32Shape)));
        return hasNoBits(binary<Word32>(&Output::sub, shape, word32(Int32Shape)), ~static_cast<uint32_t>(Int32Shape));
    }

    template<typename T> static constexpr bool isBitwiseComparable = !std::is_same_v<T, Number> && !std::is_same_v<T, OptionalNumber>;
    template<typename T> requires isBitwiseComparable<T> Word32 equal(T a, T b) { return binary<Word32>(&Output::equal, a, b); }
    template<typename T> requires isBitwiseComparable<T> Word32 notEqual(T a, T b) { return binary<Word32>(&Output::notEqual, a, b); }
    template<typename T> requires isBitwiseComparable<T> Word32 below(T a, T b) { return binary<Word32>(&Output::below, a, b); }
    template<typename T> requires isBitwiseComparable<T> Word32 belowOrEqual(T a, T b) { return binary<Word32>(&Output::belowOrEqual, a, b); }
    template<typename T> requires isBitwiseComparable<T> Word32 aboveOrEqual(T a, T b) { return binary<Word32>(&Output::aboveOrEqual, a, b); }

    Word32 logicalNot(Word32 value) { return bitXor(value, word32(1)); }
    Word32 logicalAnd(Word32 a, Word32 b) { return bitAnd(a, b); }
    Word32 logicalOr(Word32 a, Word32 b) { return a ? bitOr(a, b) : b; }
    template<typename T> T select(Word32 condition, T a, T b) { return T(m_out.select(condition.value(), a.value(), b.value())); }
    Word32 isSubsetOf(Integer, Range);

    Operand convert(Operand, const Type&, Rep);
    template<typename T> T convertTo(Operand operand, const Type& type) { return convert(operand, type, T::rep).template as<T>(); }
    Operand in(ValueID id, Rep rep)
    {
        if (rep == Rep::Double) {
            if (LValue phi = m_numberPhis.get(id.index()))
                return Operand(Rep::Double, phi);
        }
        return convert(get(id), typeOf(id), rep);
    }
    template<typename T> T in(ValueID id) { return in(id, T::rep).template as<T>(); }
    Operand convertOptional(Operand, const Type&, Rep);
    LiteralStringWord literalStringConstant(StringID);
    bool isNeverStringCell(LiteralStringWord word) const { return word.value()->hasInt64() && static_cast<uint64_t>(word.value()->asInt64()) < minCell32; }
    LValue rawWordOf(LiteralStringWord word) const
    {
        auto found = m_loadedLiteralStrings.find(word.value());
        return found == m_loadedLiteralStrings.end() ? word.value() : found->value;
    }
    Word32 isStringCell(LiteralStringWord word) { return Word32(m_out.aboveOrEqual(rawWordOf(word), m_out.constInt64(minCell32))); }
    Boxed boxLiteralString(LiteralStringWord);
    LiteralStringWord literalStringOf(Operand, const Type&);
    Operand convertLiteralString(Operand, const Type&, Rep);
    void widenLiteralStringOperands(const Value&);
    void widenLiteralString(ValueID);
    Operand narrowLiteralStringOrElse(Operand, const Type& from, const Type& to, LBasicBlock otherwise);
    std::optional<Word32> areLiteralStringsEqual(ValueID, ValueID);
    void emitBarrierForLiteralString(CellPointer owner, ValueID stored);
    bool mayBeCell(ValueID);
    Integer bitsOf(Operand);
    Operand fromBits(Integer, Rep);
    static int64_t markerBits(uint16_t kind, Rep);
    Word32 isMarker(Operand);
    Number purified(Number);
    enum class PureFunctionOfChildren : int32_t { TruncateToInteger = 1, BoxNumber, ToInt32, NonNegativeInt32, Fmod, FirstOperation };
    Integer truncateToInteger(Number);
    Word32 nonNegativeInt32OrElse(Number, LBasicBlock otherwise);
    Number remainder(ValueID dividend, ValueID divisor);
    Number fmodOnSlowPath(Number, Number);
    Number toNumber(Integer);
    Boxed box(Number value) { return Boxed(boxDouble(value.value())); }
    Boxed boxNumber(Number);
    bool tryTailCallBoxNumber(ValueID returned);
    std::optional<Number> unboxArgumentWithStub(ValueID result, const Value& cast);
    Boxed boxInt32(Word32 value) { return Boxed(Emitter::boxInt32(value.value())); }
    Boxed box(CellPointer value) { return Boxed(value.value()); }
    Number unboxedDouble(Boxed value) { return Number(unboxDouble(value.value())); }
    Word32 unboxedBoolean(Boxed value) { return Word32(unboxBoolean(value.value())); }
    Number assumedNumber(OptionalNumber value) { return Number(value.value()); }
    Integer assumedInteger(OptionalInteger value) { return Integer(value.value()); }
    CellPointer asCell(Boxed value)
    {
        LValue cell = value.value();
        if (auto found = m_decoded.find(cell); found != m_decoded.end())
            cell = found->value.cell;
        m_assumedCells.add(cell);
        return CellPointer(cell);
    }
    Integer signExtended(Word32 value) { return Integer(m_out.signExt32To64(value.value())); }
    Integer zeroExtended(Word32 value) { return Integer(m_out.zeroExt(value.value(), B3::Int64)); }
    Word32 low32(Integer value) { return Word32(m_out.castToInt32(value.value())); }
    Word32 low32(Boxed value) { return Word32(m_out.castToInt32(value.value())); }
    Word32 toInt32(Number);
    Word32 toInt32(ValueID);
    Word32 toInt32OrElse(ValueID, LBasicBlock otherwise);
    std::optional<Word32> sumToInt32(ValueID);

    TypedPointer addressOf(VariableID);
    TypedPointer inStatics(const FTL::AbstractHeap&, ptrdiff_t offset);
    TypedPointer cardOf(uint32_t offsetInStatics);
    LValue staticsBase();
    LValue cardsBase();
    void checkInitialized(Operand raw, StringID name);
    bool isKnownInitialized(VariableID) const;
    void didCheckInitialized(VariableID);
    void validateIsInitialized(Operand raw);
    void findStoresToVariables();
    Operand lowerGetModuleVariable(ValueID result, const Value&);
    TypedPointer globalPropertyOrElse(uint32_t cacheNumber, LBasicBlock miss);
    LValue globalPropertyCacheWord(uint32_t cacheNumber, size_t offset);
    Boxed globalPropertyValueOrElse(StringID, uint32_t cacheNumber, LBasicBlock miss, bool isFreeName = true);
    Boxed lowerGetGlobal(const Value&);
    std::optional<Boxed> getGlobalThisProperty(ValueID base, StringID name);
    std::optional<Boxed> getGlobalMember(ValueID base, StringID name);
    void lowerPutGlobal(const Value&);
    void lowerPutModuleVariable(const Value&);
    Operand lowerGetEnvironmentField(ValueID result, const Value&);
    void lowerPutEnvironmentField(const Value&);

    void validateIsNotEmpty(Boxed);
    void validateIsNotReleased(CellPointer environment, const Value& access);
    Word32 isCell(Boxed value)
    {
        if (auto found = m_decoded.find(value.value()); found != m_decoded.end())
            return Word32(m_out.logicalNot(m_out.below(found->value.loaded, m_out.constInt64(firstCellWord))));
        return Word32(Emitter::isCell(value.value()));
    }
    Word32 isCell(Boxed value, const Type& type) { return isX86() || type.primitives & Primitive::Number || indexOf(value.value()) ? isCell(value) : Word32(m_out.testIsZero64(value.value(), m_out.constInt64(JSValue::OtherTag))); }
    Word32 isInt32(Boxed value) { return Word32(Emitter::isInt32(value.value())); }
    Word32 isNumber(Boxed value) { return Word32(Emitter::isNumber(value.value())); }
    Word32 isBoolean(Boxed value) { return Word32(Emitter::isBoolean(value.value())); }
    Word32 isUndefinedOrNull(Boxed value)
    {
        if (auto found = m_decoded.find(value.value()); found != m_decoded.end())
            return Word32(m_out.below(found->value.loaded, m_out.constInt64(firstCellWord)));
        return Word32(isOther(value.value()));
    }
    Word32 isUndefinedOrNull(Boxed value, const Type& type)
    {
        if (!isX86() || type.primitives & Primitive::Absent || m_decoded.contains(value.value()))
            return isUndefinedOrNull(value);
        if (Options::validateAOTTypedFacts()) [[unlikely]]
            validateIsNotEmpty(value);
        return Word32(m_out.testIsZero64(value.value(), m_out.constInt64(~static_cast<int64_t>(JSValue::ValueUndefined))));
    }
    Word32 loadCellType(CellPointer);
    Word32 add(Word32 a, Word32 b) { return binary<Word32>(&Output::add, a, b); }
    Word32 isZero(Word32 a) { return unary<Word32>(&Output::isZero32, a); }
    Word32 shapeWordOf(CellPointer);
    Word32 blockHalfwordOf(CellPointer);
    Word32 orderWordOf(CellPointer);
    bool isUnmodifiedSinceAllocation(ValueID) const;
    Word32 passes(Word32 shapeWord, const ShapeTest&, LBasicBlock otherwise = nullptr);
    struct MergedShapeTest {
        ShapeTest test;
        std::optional<uint64_t> bits;
    };
    Word32 passes(Word32 shapeWord, const MergedShapeTest&, LBasicBlock otherwise = nullptr);
    std::optional<Word32> passesWithinKnownShapes(Word32 shapeWord, std::span<const MergedShapeTest>);
    std::optional<Word32> resolveWithinKnownShapes(Word32 shapeWord, std::span<const MergedShapeTest>);
    const Vector<uint32_t>& possibleShapesOf(const Type&);
    uint64_t bitsOf(const ShapeTest&) const;
    Vector<MergedShapeTest, 4> merged(Vector<ShapeTest, 4>) const;
    Word32 passesAnyOf(Word32 shapeWord, const Vector<ShapeTest, 4>&, LBasicBlock otherwise = nullptr);
    void combineRetagSourceTests(Vector<ShapeTest, 4>& sources, const Vector<LayoutID, 1>& objectLayouts) const;
    Word32 passesAnyOf(Word32 shapeWord, const Vector<LayoutID, 1>& objectLayouts, LBasicBlock otherwise = nullptr);
    void branchOnAnyShapeTest(Word32 shapeWord, const Vector<ShapeTest, 4>&, FTL::WeightedTarget passing, FTL::WeightedTarget failing);
    Word32 lowerHasShapeOf(const Value&);
    Word32 lowerIsClassObjectUnmodified(const Value&);
    void accessStaticField(const Value&, const Invocable<void(CellPointer classObject, TypedPointer slot)> auto& inlinePath, const Invocable<void(Word32 ofClass, Word32 field)> auto& genericPath);
    Operand lowerGetStaticField(ValueID result, const Value&);
    void lowerPutStaticField(const Value&);
    Word32 isClosureOf(CellPointer, FunctionID, bool mustBeUnmodified = false, bool allowsModified = false);
    Word32 lowerIsClosureOf(const Value&);
    Word32 closureFunction(CellPointer, bool isSlim);
    ptrdiff_t closureStructureIDOffset(const Function&);
    Word32 isUnreifiedSlimClosure(CellPointer);
    Word32 hasTypeOfFunctionFlag(CellPointer cell) { return hasBits(loadByte(cell, m_heaps.JSCell_typeInfoFlags), TypeOfReturnsFunction); }
    void checkClosureOfOrElse(CellPointer, FunctionID, bool mustBeUnmodified, LBasicBlock otherwise, bool allowsModified = false, bool otherwiseIsRare = false);
    void branchOnClosureOf(const Value&, LBasicBlock isOne, LBasicBlock isNone);
    void branchOnRelational(const Value&, LBasicBlock holds, LBasicBlock doesNotHold);
    bool isAwaitMayContinue(const Value&) const;
    bool isOnlyAwaited(ValueID);
    bool isReceiverGuard(const Value&) const;
    LValue plainRecordProperty(CellPointer, LayoutID, uint32_t field);
    void branchOnPlainRecord(const Value&, LBasicBlock isOne, LBasicBlock isNone);
    void convertPlainRecordOrElse(CellPointer, LayoutID, Integer depth, LBasicBlock otherwise);
    FunctionID plainRecordConverterToCall(const Type&) const;
    void callPlainRecordConverterOrElse(FunctionID, CellPointer, Integer depth, LBasicBlock otherwise);
    Word32 lowerIsPlainRecord(ValueID, const Value&);
    Operand lowerGetPlainRecordField(ValueID, const Value&);
    bool isHostObjectCacheTest(const Value&) const;
    void branchOnHostObjectCache(const Value&, LBasicBlock isCached, LBasicBlock isNot);
    ValueID receiverWithoutCasts(ValueID) const;
    void branchOnOriginalStructure(const Value&, const Terminator&, LBasicBlock isOriginal, LBasicBlock hasChanged);
    enum class GuardKind : uint8_t { Class, Structure };
    std::optional<bool> resultOfDominatingGuard(ValueID receiver, const EngineClass&, GuardKind);
    void branchOnAwaitMayContinue(const Value&, BlockID goesOn, LBasicBlock mayContinue, LBasicBlock mustSuspend);
    CellPointer awaitedObjectOrElse(ValueID awaited, LBasicBlock isNotObject);
    void findClosureTestsFusedWithBranches();

    Operand ensure(Operand, const Type& guarantee, const Type&);
    std::optional<ArgumentMoves> typeCheckThunkTests(const Type&) const;
    Boxed checkTypeWithThunk(Boxed, TypeID, ArgumentMoves&& tests);
    bool mayCastInRegister(const Type& from, const Type& to) const;
    std::optional<ArgumentMoves> castInRegisterTests(const Type&, bool usesCastTest, bool inspectsUntypedObjects) const;
    Boxed castInRegister(Boxed, Entry operation, bool operationReturnsValue, uint32_t argument, OperationEffects, ArgumentMoves&& tests, bool mayBeModifiedArray);
    bool checksRuntimeAllocatedValue(const Value&) const;
    Operand ensure(Boxed value, const Type& type) { return ensure(value, m_program.at(m_program.types.anything), type); }
    Operand ensureElement(CellPointer array, Integer index, Boxed element, const Type&);
    bool resultMayInstantiateType(const Type& guarantee, const Type&) const;
    Operand ensureProperty(CellPointer holder, StringID, Boxed returnedValue, const Type&);
    void validateSingleFunction(Operand stored, ValueID, FunctionID, bool mayBeNullish, FunctionID other = { });
    Operand narrowOrElse(Operand, const Type& from, const Type& to, LBasicBlock otherwise);
    Word32 fits(Operand, const Type& from, const Type& to);
    void throwTypeMismatch(Operand, const Type& from, const Type& to);
    CellPointer nonNullish(Operand, const Type&, StringID name);
    CellPointer nonNullish(ValueID receiver, StringID name);
    void throwNullish(Boxed, StringID name, CellPointer accessed = { });
    bool markNullishTest(CellPointer accessed);
    void validateFact(Operand, ValueID, const Type&);

    Word32 isCallable(CellPointer);
    Word32 hasClosureStructure(CellPointer, ptrdiff_t offsetOfStructureID);
    Word32 isKnownClosureStructureID(Word32);
    Word32 isClosureWithNoPropertyButPrototype(CellPointer);
    Word32 lowerTypeOfIs(const Value&);
    Word32 cellIsOfType(CellPointer, const Type& from, const Type& to, LBasicBlock otherwise);
    Word32 nonCellIsOfType(Boxed, const Type& from, const Type& to);
    Operand narrowBoxedOrElse(Boxed, const Type& from, const Type& to, LBasicBlock otherwise);
    Operand narrowBoxedToIntegerOrElse(Boxed, const Type& from, const Type& to, LBasicBlock otherwise);
    Integer integralOrElse(Number, Range, LBasicBlock otherwise);
    using Emitter::orElse;
    bool dropsGuardAt(const char* file, unsigned line) const { return !m_isValidatingFact && isDroppedForTesting(file, line); }
    void orElse(Word32 condition, LBasicBlock otherwise, const char* file = __builtin_FILE(), unsigned line = __builtin_LINE())
    {
        if (!m_isValidatingFact && isForcedForTesting(file, line)) [[unlikely]] {
            Emitter::orElse(m_out.constInt32(0), otherwise);
            return;
        }
        if (!dropsGuardAt(file, line)) [[likely]]
            Emitter::orElse(condition.value(), otherwise);
    }
    FTL::WeightedTarget rarely(LBasicBlock block, const char* file = __builtin_FILE(), unsigned line = __builtin_LINE())
    {
        if (dropsGuardAt(file, line)) [[unlikely]]
            m_out.dropNextGuardForTesting();
        if (!m_isValidatingFact && isForcedForTesting(file, line)) [[unlikely]]
            m_out.forceNextGuardForTesting();
        return FTL::rarely(block);
    }
    Operand coerce(Operand, const Type& from, const Value& cast, std::optional<OperationEffects> = std::nullopt);
    bool mayPass(const Type&, const ShapeTest&) const;
    Operand lowerCoercion(ValueID result, const Value& cast);
    Operand lowerCoercion(Operand, const Type& from, const Type& type, const Value& cast, std::optional<OperationEffects> = std::nullopt);
    Operand lowerNonNull(ValueID result, const Value& cast);
    Operand lowerAuditedCast(ValueID result, const Value& cast);
    void auditResult(Operand, const Value&);
    Boxed assertionFailed(Operand, const Type& from, TypeID wanted, uint32_t what, ValueID storedTo = { });
    Word32 auditLineAndColumn();
    ValueID nextStoreReceiver(ValueID stored) const;
    Operand lowerCast(ValueID result, const Value&);
    std::optional<Operand> castInOutlinedBody(const Value& cast, Operand, const Type& from, const Type& to);
    Word32 typeAndReason(TypeID, CastReason);
    unsigned useCountOf(ValueID);
    void findUsers();
    std::span<const ValueID> usersOf(ValueID);
    std::optional<unsigned> iteratorResultSlot(ValueID object, StringID name);
    bool isUsedByTerminator(ValueID);
    std::pair<uint32_t, uint32_t> placeOf(ValueID);
    bool isErrorThrownAtCreation(const Block&, ValueID);
    bool isErrorThrownToIgnoringHandler(const Block&, ValueID);
    void throwNewError(const Value&);
    struct PinnedRead {
        CellPointer cell;
        Word32 shapeWord;
        Word32 isModified;
        LayoutID supertype;
        StringID name;
        Word32 index;
        Vector<StringID, 4> texts;
    };
    bool isOnlyComparedWithStringsBeforeStores(ValueID read, Vector<ValueID, 4>& stringsByIndex);
    TypedPointer pinnedFieldsGuard(SlotID);
    Word32 hasShapePinnedTo(const PinnedRead&, StringID text);
    Word32 isPinnedTo(const PinnedRead&, ValueID constant);
    void guardPinnedFields(LayoutID holder, ValueID object, const Field&, ValueID stored);
    bool mayFuseWithRead(size_t readIndex);
    bool readsLengthOfPredictedArrayOrString(size_t readIndex);
    std::optional<ValueID> elementReadAfterLengthCheck(ValueID array);
    bool isFollowedByInBoundsElementRead(size_t index);
    unsigned numberOfFollowingCasts(size_t index);
    unsigned fusedCastCount(size_t readIndex);
    std::optional<size_t> fusedUseIndex(size_t readIndex, unsigned numberOfCasts);
    unsigned numberOfCastsFusedAfterUse(size_t useIndex);
    unsigned numberOfCastsFusedAfterArithmetic(size_t index);
    struct ReadFusedWithUse {
        size_t readIndex;
        unsigned numberOfCasts;
    };
    std::optional<ReadFusedWithUse> readFusedWithUse(size_t useIndex);
    Operand lowerReadFusedWithUse(ReadFusedWithUse, size_t useIndex, unsigned numberOfCastsAfterUse);
    std::optional<Operand> lowerFusedCast(size_t index);
    std::optional<Operand> lowerFusedUse(size_t index);
    std::optional<Operand> numberAndValueArithmeticWithStub(const Value&, const Type&, OperationEffects);
    Operand lowerFusedCasts(Operand, const Type& from);
    Operand placeholder(Rep);
    Operand thrown(Operand, const Type&);
    Boxed thrownNullish(Boxed);
    struct SunkCoercion {
        ValueID cast;
        Vector<const ShapeTest*, 4> sources;
    };
    std::optional<SunkCoercion> sunkCoercionAt(size_t useIndex);
    bool coercionSinks(size_t castIndex);
    bool coercionIsOnlyRead(size_t castIndex);
    bool usesCastTest(const Value& cast) const;
    void branchOnTransitionSource(CellPointer, const SunkCoercion&, FTL::WeightedTarget isSource, FTL::WeightedTarget isNotSource);
    void carryOut(CellPointer, const SunkCoercion&);
    Operand lowerCastAfterCoercion(ValueID result, const Value&, const SunkCoercion&);
    Word32 lowerIsUndefinedOrNull(ValueID);
    Word32 lowerToBoolean(ValueID);
    Word32 lowerIsInstanceOfClass(const Value&);
    bool instanceOfUsesShapeTest(const Value&);
    bool isOnlyUsedByShapeInstanceOf(ValueID);
    Word32 lowerInstanceOfByShape(const Value&);
    const EngineClass* instanceOfEngineClass(const Value&);
    void findInstanceOfRuns(const Vector<BlockID>& order);
    Word32 isNonObjectOrPlainObject(ValueID);
    Word32 lowerInstanceOfEngineClass(ValueID, const Value&, const EngineClass&);
    Word32 lowerGenericInstanceOf(Boxed value, Boxed constructor);
    bool isUsedOnlyByInlineInstanceOf(ValueID);

    Number asNumericOperand(ValueID);
    bool hasNumericType(ValueID) const;
    Operand numericResult(Operand computed, const Type& guarantee, const Type&, bool typeIsFact);
    Operand lowerBinary(ValueID result, const Value&);
    Operand lowerNumericBinary(Opcode, ValueID a, ValueID b, const Type& result);
    Operand int32ResultOrElse(Integer, const Type& result, LBasicBlock otherwise);
    Word32 int32OrElse(Opcode, Word32, Word32, LBasicBlock otherwise, const char* file = __builtin_FILE(), unsigned line = __builtin_LINE());
    Operand lowerInt32Binary(Opcode, ValueID a, ValueID b, const Type& result, LBasicBlock otherwise);
    Integer lowerBitwise(Opcode, ValueID a, ValueID b);
    Operand lowerUnary(ValueID result, const Value&);
    Word32 lowerRelational(const Value&);
    Integer integerBound(Opcode, Number);
    std::optional<Word32> lowerRelationalWithIntegerBound(const Value&);
    std::optional<Word32> lowerRelationalWithLikelyInteger(const Value&);
    std::optional<Word32> lowerRelationalWithOptionalInteger(const Value&);
    Integer clampToMaximum(Integer, int64_t limit);
    Integer clampToMinimum(Integer, int64_t limit);
    Word32 lowerStrictEq(ValueID a, ValueID b);
    static bool toBooleanNeedsStub(const Type&);
    Word32 isTruthy(CellPointer, const Type&);
    Word32 isTruthy(Boxed, const Type&);
    void findTruthinessPhis();
    void findLazilyFetchedLeaves();
    void findSplitJoinParameters();
    unsigned integerComparisonCount(ValueID);
    bool isUsedAsInt32(ValueID);
    ValueID currentValue() const;
    Word32 toInt32OnPath(Number, bool isRare);
    Integer exactIntegerOnPath(Number);
    void didDefineNumber(ValueID, const Value&, Number);
    std::optional<Integer> knownIntegerFor(ValueID);
    using FirstRepeatedComparisonMap = UncheckedKeyHashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;
    template<typename Qualifies> void findRepeatedComparisons(const Vector<BlockID>& order, FirstRepeatedComparisonMap&, unsigned minCount, const Qualifies&);
    void findRepeatedIntegerComparisons(const Vector<BlockID>& order);
    void findRepeatedShortStringComparisons(const Vector<BlockID>& order);
    std::optional<Word32> compareWithCachedShortString(ValueID a, ValueID b);
    Integer integerEqualTo(Boxed);
    std::optional<Word32> compareWithCachedInteger(ValueID a, ValueID b);
    Word32 lowerLooseEq(ValueID a, ValueID b);
    Word32 genericToBoolean(ValueID);
    void branchOnTruthinessWithStub(Boxed, FTL::WeightedTarget isTruthy, FTL::WeightedTarget isFalsy);
    CellPointer toStringCell(ValueID);

    enum LoadFlag : uint8_t {
        IsSafe = 1,
        IsFixed = 2,
    };
    LValue marked(LValue load, unsigned flags);
    template<typename T, typename Base> T loadField(Base, const B3::AbstractHeap&, unsigned flags = 0);
    template<typename Base> Word32 loadByte(Base base, const B3::AbstractHeap& field, unsigned flags = 0) { return Word32(marked(m_out.load8ZeroExt32(base.value(), field), flags)); }
    template<typename T, typename Base> void storeField(Base, const B3::AbstractHeap&, T);
    Word32 hasNoBits(Pointer a, uintptr_t mask) { return Word32(m_out.testIsZeroPtr(a.value(), m_out.constIntPtr(mask))); }
    Pointer offsetBy(Pointer base, Integer index, unsigned logSize) { return Pointer(m_out.add(base.value(), logSize ? m_out.shl(index.value(), m_out.constInt32(logSize)) : index.value())); }
    uint32_t minObjectSizeOf(LayoutID) const;
    unsigned safeIfReaches(CellPointer, unsigned end) const;
    unsigned safeIfAllSubtypesHave(CellPointer, const Field&) const;
    Operand load(Store, TypedPointer, unsigned flags, LValue holder = nullptr);
    Boxed boxDecoded(LValue index, LValue cell, LValue loaded = nullptr);
    Boxed rematerializedValue(Boxed);
    struct Parts {
        LValue word;
        LValue cell;
        bool zeroReadsUndefined;
    };
    static constexpr int64_t firstCellWord = undefinedCell32 + 1;
    Parts partsOf(Boxed);
    Boxed fromParts(LValue word, LValue cell, bool zeroReadsUndefined);
    LValue indexOf(LValue boxed) const
    {
        auto found = m_decoded.find(boxed);
        return found == m_decoded.end() ? nullptr : found->value.index;
    }
    LValue indexOrBits(Boxed boxed) const
    {
        LValue index = indexOf(boxed.value());
        return index ? index : boxed.value();
    }
    Word32 haveEqualBits(Boxed a, Boxed b)
    {
        auto isNotCell = [](LValue value) { return value->hasInt64() && static_cast<uint64_t>(value->asInt64()) <= undefinedCell32; };
        auto isNull = [](LValue value) { return value->hasInt64() && value->asInt64() == nullCell32; };
        if (isNull(a.value()) || isNull(b.value())) {
            auto wordOrBits = [&](Boxed boxed) {
                auto found = m_decoded.find(boxed.value());
                return found == m_decoded.end() ? boxed.value() : found->value.loaded;
            };
            return Word32(m_out.equal(wordOrBits(a), wordOrBits(b)));
        }
        if (isNotCell(a.value()) || isNotCell(b.value()))
            return Word32(m_out.equal(indexOrBits(a), indexOrBits(b)));
        auto indexAndCellFromNarrowSlot = [&](Boxed boxed) -> std::pair<LValue, LValue> {
            if (auto found = m_decoded.find(boxed.value()); found != m_decoded.end())
                return m_cellIndices.contains(found->value.cell) ? std::pair { found->value.index, found->value.cell } : std::pair<LValue, LValue> { };
            if (auto found = m_cellIndices.find(boxed.value()); found != m_cellIndices.end())
                return { found->value, boxed.value() };
            return { };
        };
        auto [indexOfA, cellOfA] = indexAndCellFromNarrowSlot(a);
        auto [indexOfB, cellOfB] = indexAndCellFromNarrowSlot(b);
        if (indexOfA && indexOfB) {
            Word32 result(m_out.equal(indexOfA, indexOfB));
            ensureStillAliveHereForWord(CellPointer(cellOfA), indexOfA);
            ensureStillAliveHereForWord(CellPointer(cellOfB), indexOfB);
            return result;
        }
        return equal(a, b);
    }
    LValue cellCageBase() { return m_cellCageBase ? m_cellCageBase : marked(fixedPointer(Instance::offsetOfCellCageBase()), IsFixed | IsSafe); }
    LValue decodeCell32(LValue index) { return cell32IsPointer ? index : m_out.add(cellCageBase(), m_out.shl(index, m_out.constInt32(cell32Shift))); }
    Word32 refersToNumberBox(Word32 word) { return below(Word32(m_out.sub(word.value(), m_out.constInt32(nullInt32 + 1))), word32(static_cast<uint32_t>(minInlineInt32) - static_cast<uint32_t>(nullInt32 + 1))); }
    TypedPointer numberBoxAt(const Field& field, Word32 word) { return m_out.address(heapOf(field.slot), m_out.add(marked(fixedPointer(Instance::offsetOfNumberBoxBase()), IsFixed | IsSafe), m_out.shl(m_out.zeroExtPtr(m_out.bitAnd(word.value(), m_out.constInt32(INT32_MAX))), m_out.constInt32(numberBoxShift))), 0); }
    void unlessBoxed(CellPointer, const Layout&, LBasicBlock otherwise);
    LValue closureTableEntry(uint32_t general) { return m_out.load32(m_out.address(m_heaps.AOTTyped_runtimeState, fixedPointer(Instance::offsetOfTypedClosures()), static_cast<ptrdiff_t>(general) * sizeof(uint32_t))); }
    LValue closureIfCreated(uint32_t general)
    {
        if (Options::useAOTWideReferences())
            return m_out.load64(m_out.address(m_heaps.AOTTyped_runtimeState, fixedPointer(Instance::offsetOfTypedClosures()), static_cast<ptrdiff_t>(general) * sizeof(void*)));
        return m_out.add(noClosure(), m_out.shl(m_out.zeroExtPtr(closureTableEntry(general)), m_out.constInt32(cell32Shift)));
    }
    LValue noClosure() { return Options::useAOTWideReferences() ? m_out.constInt64(0) : m_out.add(cellCageBase(), m_out.constInt64(1 << cell32Shift)); }
    Word32 isSingletonClosure(CellPointer cell, uint32_t general)
    {
        if (auto found = m_cellIndices.find(cell.value()); found != m_cellIndices.end())
            return Word32(m_out.equal(m_out.sub(m_out.castToInt32(found->value), m_out.int32One), closureTableEntry(general)));
        return equal(cell, CellPointer(closureIfCreated(general)));
    }
    void validateCell32(LValue index, LValue stored, bool mayBeNullish);
    void store(Store, Operand, TypedPointer);
    const B3::AbstractHeap& heapOf(SlotID slot) { return m_heaps.AOTTyped_slots[slot.index()]; }
    void barrier(CellPointer owner);
    UncheckedKeyHashMap<LValue, LValue> m_loadedLiteralStrings;
    UncheckedKeyHashMap<LValue, LValue> m_boxedLiteralStrings;
    UncheckedKeyHashMap<LValue, LValue> m_unboxedLiteralStrings;
    struct EmittedBarrier {
        LValue owner { nullptr };
        LValue test { nullptr };
        LBasicBlock branchBlock { nullptr };
        LBasicBlock next { nullptr };
        size_t indexInNext { 0 };
    };
    bool allPathsLeadHere(LBasicBlock, size_t index, Vector<LBasicBlock, 8>& inProgress, Vector<LBasicBlock, 8>& walked) const;
    void deferLastBarrierTo(CellPointer owner);
    EmittedBarrier m_lastBarrier;
    RefPtr<B3::StackmapGenerator> m_emptyGenerator { createSharedTask<B3::StackmapGeneratorFunction>([](CCallHelpers&, const B3::StackmapGenerationParams&) { }) };
    bool isFollowedByStoreWithBarrier(ValueID environment) const;
    bool defersBarrierToPark(ValueID frame) const;
    ValueID m_environmentAwaitingBarrier;
    bool m_isFillingReusedEnvironment { false };
    void barrier(CellPointer array, std::span<const Boxed> stored);
    void fenceAfterInitialization();
    void didLowerInitializingStore();
    BitVector m_fieldsAwaitingFirstStore;
    bool m_isReportingFactViolation { false };
    LayoutID m_fieldsAwaitingFirstStoreLayout;

    Word32 isAbsent(Operand raw);
    bool isLoweringInitializingStore() const { return isInitializingStore(m_program, m_function, *m_currentBlock, m_currentIndexInBlock); }
    const Field* fieldFor(const Type& receiver, StringID) const;
    const Field* predictedFieldFor(const Type& expected, StringID, bool mayBeOutOfLine = false) const;
    CellPointer withFieldOrElse(ValueID object, LayoutID supertype, const Field&, LBasicBlock otherwise, bool mayBeAbsent = false);
    bool hasSubtypeKeepingField(LayoutID supertype, StringID) const;
    bool hasSubtypeWithField(LayoutID supertype, StringID) const;
    bool mayBeFrozen(LayoutID supertype) const;
    Operand loadSlot(CellPointer, const Field&, unsigned flags);
    Operand readField(CellPointer, const Field&, const Type&);
    Operand readProperty(CellPointer, LayoutID supertype, const Field&, const Type&);
    Operand getFromObjectWithExtraProperties(CellPointer, StringID name, const Type&);
    Word32 hasPrototypeOfLayout(CellPointer);
    Word32 hasOtherPrototype(CellPointer);
    bool isPrivateNameOfOneInstance(StringID) const;
    LValue instanceOwning(CellPointer);
    Word32 belongsToThisInstance(CellPointer);
    void throwIfOfAnotherInstance(CellPointer, StringID privateName);
    void branchIfOwnFieldRequested(CellPointer, LBasicBlock isNotRequested, LBasicBlock asks);
    Word32 hasField(CellPointer, const Field&);
    Word32 hasOwnField(CellPointer, const Field&);
    bool storeNeedsNoStorage(const Field&, ValueID stored) const;
    Word32 isFieldAbsent(CellPointer, const Field&, unsigned flags);
    void branchOnInt32Inline(Integer signExtendedWord, FTL::WeightedTarget inlinePath, FTL::WeightedTarget otherwise);
    struct InitialStores {
        size_t first;
        unsigned count;
        uint32_t orderWord;
        const Type* thisType;
    };
    std::optional<InitialStores> initialStoresAt(size_t index) const;
    void lowerInitialStores(const InitialStores&);
    struct ObjectLiteral {
        size_t allocation;
        bool hasNullPrototype;
        Vector<size_t, 6> fields;
        unsigned slot { 0 };
        uint32_t row { Format::none };
        unsigned inlineCapacity { 0 };
        ValueID newTarget { };
        ValueID constructor { };
        std::optional<size_t> describes;
        std::optional<size_t> descriptor;
    };
    bool isFilledInLoopByComputedKey(ValueID object) const;
    std::optional<InlineBuiltin> enumeratingBuiltinOf(ValueID list, unsigned depth = 0) const;
    ValueID listOfElementRead(ValueID element, bool mustBeFirstElement) const;
    bool isEnumeratedKey(ValueID key, unsigned depth = 0) const;
    bool isDefineProperty(const Value&) const;
    std::optional<size_t> definePropertyUsingDescriptor(const ObjectLiteral&);
    void lowerPropertyDefinition(const ObjectLiteral&);
    bool isDefineField(const Value&) const;
    bool isPrivateFieldAdd(const Value&) const;
    struct AccessorGroup {
        Vector<size_t, 8> members;
        LValue buffer { nullptr };
    };
    bool isAccessorDefinition(const Value&) const;
    AccessorGroup* accessorGroupAt(size_t index);
    void lowerMemberOfAccessorGroup(AccessorGroup&, const Value&);
    struct SpreadLiteral {
        size_t allocation;
        Vector<size_t, 8> members;
    };
    SpreadLiteral* spreadLiteralAt(size_t index);
    void lowerSpreadLiteral(const SpreadLiteral&);
    bool emitDefineByName(Boxed object, StringID, Boxed value);
    bool isObjectCreateNull(const Value&) const;
    ObjectLiteral* objectLiteralAt(size_t index);
    static bool mayDeferObjectLiteral(const ObjectLiteral& literal) { return !literal.describes && !literal.descriptor && !literal.newTarget && !literal.hasNullPrototype && !literal.fields.isEmpty(); }
    bool tryDeferObjectLiteral(const ObjectLiteral& literal) { return mayDeferObjectLiteral(literal) && tryDeferLiteralAllocation(m_currentBlock->values[literal.allocation], literal.fields.span()); }
    CellPointer countPlainAllocation(PlainAllocationKind, ValueID, CellPointer);
    struct CloneGroup {
        size_t clone;
        Vector<size_t, 4> stores { };
        size_t cast { notFound };
        Vector<LayoutID, 2> layouts { };
        bool isDecidedAtClone { false };
        LValue decision { nullptr };
        LValue copied { nullptr };
        size_t last() const { return cast != notFound ? cast : stores.last(); }
    };
    std::optional<CloneGroup> findCloneGroup(size_t cloneIndex);
    struct TypedCloneOfUntypedSource {
        const Block* block { nullptr };
        ValueID copy;
        LayoutID layout;
        size_t firstAdjacentStore { notFound };
        size_t cast { notFound };
        bool plainObjectsPassCast { false };
        Operand castResult { };
    };
    std::optional<uint64_t> typedCloneTargetAt(size_t cloneIndex);
    void storeToTypedCloneField(CellPointer, const Value& store);
    void storeToPlainClone(Boxed, const Value& store);
    bool tryStoreToTypedClone(const Value& store);
    std::optional<Operand> tryCastTypedClone();
    TypedCloneOfUntypedSource m_typedCloneOfUntypedSource;
    bool allSubtypesAreIn(const Type&, const Vector<LayoutID, 2>& layouts);
    void branchOnSourceLayout(Word32 shapeWord, const Layout&, bool isLastCandidate, LBasicBlock otherwise);
    template<typename Functor> void dispatchOnCloneSourceLayout(const CloneGroup&, LBasicBlock slowPath, const Functor& onLayout);
    void decideCloneGroup(CloneGroup&);
    bool everyCloneOffUsualPathPassesCast(const CloneGroup&);
    std::optional<uint64_t> namesStoredWhereEveryLayoutIsCopiedInline(const CloneGroup&);
    std::optional<CellPointer> tryCloneAsExtendedPlainObject(const CloneGroup&);
    Operand lowerCloneGroup(CloneGroup);
    CellPointer copyRecordAs(const Value&, LayoutID to, CopyNamesKind, const Vector<LayoutID, 4>* inlineSources = nullptr, bool callerInitializesNamedFields = false, LBasicBlock onFailure = nullptr);
    Operand initialFieldValue(const Field&, bool wasAllocated);
    CellPointer copyRecord(CellPointer, Word32 orderWord, const Layout&, const Vector<StringID, 4>& overwritten);
    std::optional<CloneGroup> m_cloneGroup;
    struct MergeGroup {
        size_t first;
        LayoutID layout;
        bool startsWithClone;
    };
    std::optional<MergeGroup> findMergeGroup(size_t firstMemberIndex);
    Operand lowerMergeGroup(MergeGroup);
    std::optional<MergeGroup> m_mergeGroup;
    struct SpreadGroup {
        size_t first;
        size_t cast;
        Vector<size_t, 4> definitions;
        Vector<LayoutID, 4> layouts;
        bool startsWithClone { false };
    };
    std::optional<SpreadGroup> findSpreadGroup(size_t spreadIndex);
    std::optional<SpreadGroup> findSpreadGroupAfterClone(size_t copyIndex);
    bool canStoreFieldsDirectly(const Layout&, const SpreadGroup&);
    Operand lowerSpreadGroup(SpreadGroup);
    std::optional<SpreadGroup> m_spreadGroup;
    struct CastOfCopiedRecord {
        const Block* block { nullptr };
        size_t cast { notFound };
        Operand result { };
    };
    CastOfCopiedRecord m_castOfCopiedRecord;
    bool m_isLoweringMergeGroup { false };
    bool isPredictedSymbol(const Function&, ValueID) const;
    void lowerObjectLiteral(const ObjectLiteral&);
    static unsigned roundInlineCapacityToCellSize(unsigned inlineCapacity);
    static unsigned inlineCapacityForLiteral(const Program&, const Function&, ValueID allocationSite, bool hasFields, const UsersInBlocks* users = nullptr);
    const UsersInBlocks* usersOfAllocation(ValueID allocationSite) const;
    mutable bool m_didFindAllocationUsers { false };
    mutable BitVector m_isAllocation;
    mutable UncheckedKeyHashMap<uint32_t, UsersInBlocks, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_allocationUsers;
    const UsersInBlocks m_noUsers;
    static void assignStructureDescriptors(Program&);
    friend void assignStructureDescriptors(Program&);
    char m_literalPath { '?' };
    CellPointer newObjectWithProperties(unsigned slot, bool hasNullPrototype, ValueID newTarget, ValueID constructor, unsigned inlineCapacity, std::span<const StringID> names, std::span<const Boxed> values, uint64_t stores = 0, const ObjectLiteral* group = nullptr, uint32_t row = Format::none);
    uint32_t structureDescriptorFor(std::span<const StringID> names, unsigned inlineCapacity, bool hasNullPrototype) const;
    void findStructureDescriptorOf(ObjectLiteral&) const;
    CellPointer newObjectLiteral(std::span<const StringID> names, std::span<const Boxed> values);
    uint32_t rowOfNames(std::span<const StringID>);
    uint32_t rowOf(std::span<const uint32_t>);
    struct CloneDefinitions {
        uint64_t more;
        Vector<ValueID, 2> values;
    };
    CellPointer finishObjectLiteral(CellPointer);
    struct CloneCapacity {
        ValueID copy;
        LayoutID supertype;
        Vector<LayoutID, 4> subtypesWithoutCapacity;
    };
    CloneCapacity cloneCapacityOf(ValueID copy);
    bool alwaysPreservesSourceLayout(const CloneCapacity&) const;
    Word32 preservesSourceLayout(ValueID source, const CloneCapacity&);
    CellPointer cloneObjectOutOfLine(ValueID source, const CloneCapacity&);
    CellPointer cloneObject(ValueID source, bool keepsLayout, const CloneDefinitions& = { }, const CloneCapacity& = { });
    CloneDefinitions definitionsOnClone(ValueID object, bool mayCarryOut);
    CloneDefinitions storesDirectlyBehindClone(ValueID object);
    const Block* m_cloneDefinitionsBlock { nullptr };
    Vector<size_t, 2> m_cloneDefinitions;
    Vector<LayoutID, 2> inlineClonedLayouts(const Type&) const;
    bool populatesFirstArgument(const Value&) const;
    std::optional<size_t> indexOfAllocatingUse(ValueID) const;
    bool isUnreachableFromHandler(ValueID);
    mutable const Block* m_newObjectIndicesBlock { nullptr };
    mutable UncheckedKeyHashMap<uint32_t, size_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_newObjectIndices;
    bool isAllocatedByFirstUse(ValueID id) const { return !!indexOfAllocatingUse(id); }
    bool isAllocatedHere(ValueID id) const { return indexOfAllocatingUse(id) == m_currentIndexInBlock; }
    void defineDeferredAllocation(ValueID allocation, CellPointer);
    using NamedStores = Vector<size_t, 4>;
    const NamedStores* namedStoresAt(size_t index);
    void lowerNamedStores(const NamedStores&);
    using StoresAfterSingleTest = Vector<size_t, 4>;
    const Field* fieldOfStoreAfterSingleTestAt(size_t index);
    const StoresAfterSingleTest* storesAfterSingleTestAt(size_t index);
    void lowerStoresAfterSingleTest(const StoresAfterSingleTest&);
    struct PropertyRunStore {
        size_t index;
        ValueID object;
        StringID name;
        ValueID key;
        ValueID stored;
        bool isDefinition;
    };
    struct PropertyRun {
        Vector<PropertyRunStore, 8> stores;
        Vector<size_t, 8> elementStores;
        bool mayBePrototype { false };
        bool mayReplace { false };
        bool mayFindExistingProperties { false };
        Vector<unsigned, 8> closureStoresInRun;
        bool keysAreFixed { false };
        struct Piece {
            unsigned first;
            unsigned count;
            unsigned slot;
        };
        Vector<Piece, 2> pieces;
        unsigned slot { 0 };
    };
    ValueID withoutUpcasts(ValueID) const;
    bool isLazyClosureStore(const Value& closure, const Value& store);
    void putLazyClosure(Boxed holder, StringID, FunctionID);
    std::optional<FunctionID> lazyClosureStoredBy(const Value& store);
    bool isStoredAsLazyClosure(ValueID closure);
    bool isLazilyCreated(ValueID closure);
    bool isCalledWithoutAllocation(ValueID read);
    Word32 isUnmaterializedClosure(Boxed boxed) { return Word32(m_out.equal(indexOrBits(boxed), m_out.constInt64(unmaterializedClosureCell32))); }
    CellPointer materializeLazyClosure(CellPointer, const Field&);
    void materializeLazyClosureIfNeeded(CellPointer, const Field&);
    bool m_readsRawField { false };
    const Value* lazyClosureStoreAfterJump(const Block& from);
    bool mayBeStoredBeforeJump(const Value& store);
    std::optional<PropertyRunStore> propertyRunStoreAt(size_t index);
    bool mayInterleaveWithPropertyRun(size_t index);
    std::optional<FunctionID> functionOfPrototypeRead(ValueID) const;
    const PropertyRun* propertyRunAt(size_t index);
    bool mayBeCreatedWithPropertyRun(ValueID closure);
    std::optional<Boxed> lowerPrivateAccessAtSite(const Value&, bool isStore);
    bool isCreatedWithPropertyRun(ValueID closure);
    bool isKeyReadOnPropertyRunMiss(ValueID variableRead);
    void lowerPropertyRun(const PropertyRun&, const Vector<Boxed, 8>* currentValues = nullptr);
    Operand readInt32OrNumberBox(CellPointer, const Field&, const Type&);
    bool isStoredInline(const Field&, ValueID stored) const;
    std::optional<uint32_t> fieldInitializedBy(LayoutID, const Value& store) const;
    bool mayHaveCollectedSinceAllocation(ValueID object) const;
    void storeInt32OrNumberBox(CellPointer, const Field&, ValueID stored, bool isFirstStore, LBasicBlock otherwise = nullptr);
    bool isInitializedWith(const Allocation&, StringID name) const;
    mutable const Allocation* m_initializedFieldsAllocation { nullptr };
    mutable BitVector m_initializedFields;
    void didCreateFieldInCell(CellPointer, const Field&, OperationEffects, const ScopedLambda<void()>& onCacheMiss);
    uint32_t extentOfFollowingStores(const Field&);
    struct FieldCreationRun {
        Vector<size_t, 8> stores;
        Vector<const Field*, 8> fields;
    };
    const Field* fieldOfStoreAfterShapeTestOnlyAt(size_t index, ValueID object);
    bool isSideEffectFreeFieldRead(size_t index);
    bool m_isAfterStoreShapeTest { false };
    bool m_receiverIsNotOfAnotherInstance { false };
    std::optional<FieldCreationRun> fieldCreationRunAt(size_t index, ValueID object);
    void createFieldsInRun(CellPointer, const FieldCreationRun&);
    bool isCreatedWithRun(CellPointer, ValueID object);
    void didCreateFieldsInRun(CellPointer, uint64_t slotKeys, LValue cache);
    const Block* m_fieldCreationRunBlock { nullptr };
    Vector<size_t, 8> m_storesInFieldCreationRun;
    void writeField(CellPointer, ValueID object, const Field&, ValueID stored);
    bool isAllocatedAsSubtypeOf(ValueID object, LayoutID supertype) const;
    bool isStillUnescaped(ValueID allocation) const;
    bool mayDespecialize(LayoutID supertype) const;
    template<typename Has, typename Lacks> Operand accessThroughSupertype(CellPointer, ValueID object, LayoutID supertype, const Field&, JoinRep, const Has&, const Lacks&);
    static constexpr unsigned maxSubtypesWithOwnFieldPath = 4;
    template<typename InOwnField, typename Otherwise> Operand dispatchOnSubtypeField(CellPointer, LayoutID supertype, const Field&, JoinRep, const InOwnField&, const Otherwise&);
    template<typename InlinePath, typename GenericPath> Operand ifSubtype(CellPointer, ValueID object, LayoutID supertype, JoinRep, const InlinePath&, const GenericPath&);
    struct KnownSubtype {
        LValue cell { nullptr };
        LayoutID supertype;
        LValue isSubtype { nullptr };
        LBasicBlock block { nullptr };
        unsigned index { 0 };
    };
    KnownSubtype m_knownSubtype;
    bool mayHaveCalledSince(LBasicBlock, unsigned index, unsigned numberOfBlocks);
    template<typename Passes, typename Fails> Operand dispatchOnShape(CellPointer, const ShapeTest&, JoinRep, const Passes&, const Fails&);
    Operand lowerGetField(ValueID result, const Value&);
    void lowerPutField(ValueID, const Value&);
    Operand lowerGetById(ValueID result, const Value&);
    void lowerPutById(ValueID, const Value&);
    Word32 lowerInById(const Value&);
    Word32 emitInByVal(Boxed object, Boxed key);
    struct PresenceTestBeforeRead {
        B3::Variable* valueIfOwn { nullptr };
        BitVector blocksThatForgetAtStart;
        BitVector blocksThatForgetAtEnd;
    };
    Vector<PresenceTestBeforeRead, 1> m_presenceTestsBeforeReads;
    UncheckedKeyHashMap<uint32_t, unsigned, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_presenceTestOf;
    void findPresenceTestsBeforeReads();
    void forgetValuesFoundByPresenceTests(bool isAtStart);
    B3::Variable* valueFoundByPresenceTest(ValueID) const;
    Word32 lowerPresenceTestBeforeRead(const Value&, B3::Variable* valueIfOwn);
    Operand lowerNewObject(ValueID, const Value&);
    CellPointer stackAllocatedCell(size_t);
    CellPointer lowerMaterializeEnvironment(const Value&);
    Word32 extensionWordOf(CellPointer cell) { return loadField<Word32>(cell, m_heaps.AOTTypedObject_extension); }
    Pointer extensionAt(Word32 word) { return Pointer(decodeCell32(m_out.zeroExtPtr(word.value()))); }
    Word32 extensionCovers(CellPointer, const Field&);
    TypedPointer fieldAt(CellPointer cell, const Field& field) { return m_out.address(heapOf(field.slot), field.isOutOfLine ? extensionAt(extensionWordOf(cell)).value() : cell.value(), field.offset); }

    std::optional<LoweringContext::SitePosition> frameRemainingAfterTailCall(const Value& call) const
    {
        if (m_function.isSharedOutlinedBody)
            return std::nullopt;
        for (uint32_t frame = call.inlineFrame; frame; frame = m_function.inlineFrames[frame].parent) {
            auto& inlinedCall = m_function.inlineFrames[frame];
            if (!inlinedCall.isTailCall)
                return LoweringContext::SitePosition { inlinedCall.callPosition, inlinedCall.parent };
        }
        return std::nullopt;
    }
    uint32_t nextSite()
    {
        if (m_activeHandler)
            ++m_callSitesAwaitingLandingPad;
        LoweringContext::SitePosition where { m_function.position, 0 };
        if (m_currentBlock && !m_currentBlock->values.isEmpty()) {
            auto& value = m_function.at(m_currentBlock->values[std::min<size_t>(m_currentIndexInBlock, m_currentBlock->values.size() - 1)]);
            where = { value.position, value.inlineFrame, value.opcode == Opcode::CallDirect && value.constructs, m_parameterCheckCallee ? m_parameterCheckCallee : m_currentIndexInBlock < m_currentBlock->values.size() ? value.parameterCheckCallee() : FunctionID() };
            if (value.isInTailPosition && !m_isBeforeTailCall) {
                if (auto remaining = frameRemainingAfterTailCall(value)) {
                    where.position = remaining->position;
                    where.inlineFrame = remaining->inlineFrame;
                } else if (size_t index = m_context.framesDroppedByTailCalls.find(value.inlineFrame); index != notFound)
                    where.inlineFrame = static_cast<uint32_t>(m_context.inlineCallSites.size() - m_context.framesDroppedByTailCalls.size() + index);
            }
        }
        if (m_throwPosition)
            where = *m_throwPosition;
        m_context.sitePositions.append(where);
        uint32_t bits = CallSiteIndex(BytecodeIndex(m_nextSite++)).bits();
        m_lastSite = m_context.inlineCallSites.isEmpty() ? bits : PackedSite::pack(where.inlineFrame, bits);
        return m_lastSite;
    }
    uint32_t nextSiteWithCastItem(std::optional<size_t> castIndex)
    {
        uint32_t number = m_nextSite;
        uint32_t site = nextSite();
        if (castIndex) {
            auto& cast = m_function.at(m_currentBlock->values[*castIndex]);
            m_context.sitePositions.append({ cast.position, cast.inlineFrame, false, m_parameterCheckCallee ? m_parameterCheckCallee : cast.parameterCheckCallee() });
            m_context.itemSites.append({ number, 0, PackedSite::pack(cast.inlineFrame, CallSiteIndex(BytecodeIndex(m_nextSite++)).bits()) });
        }
        return site;
    }
    uint32_t nextItemSite()
    {
        auto& value = m_function.at(m_currentBlock->values[m_currentIndexInBlock]);
        m_context.sitePositions.append({ value.position, value.inlineFrame });
        return PackedSite::pack(value.inlineFrame, CallSiteIndex(BytecodeIndex(m_nextSite++)).bits());
    }
    LValue callEntry(Entry, LType, bool mayThrow, OperationEffects, const Vector<LValue, 8>& arguments);
    uint32_t throwAt(ThrowKind, std::optional<Operand>, uint32_t a, std::optional<uint32_t> b = std::nullopt, unsigned newErrorClassPlusOne = 0);
    void requireOrThrowAt(Word32 condition, ThrowKind, std::optional<Operand>, uint32_t a);
    using FastPathChecks = SharedTask<CCallHelpers::JumpList(CCallHelpers&, GPRReg result, std::span<const GPRReg> checked)>;
    bool canCallOnLatePath(Entry, bool mayThrow, const Vector<LValue, 8>& arguments) const;
    LValue callOnLatePath(Entry, bool mayThrow, OperationEffects, const Vector<LValue, 8>& arguments, const Vector<LValue, 4>& checked, Ref<FastPathChecks>&&, LValue fastValue = nullptr, std::optional<unsigned> firstAccessedIndex = std::nullopt);
    OperationEffects builtinCallEffects(const Value&);
    void applyEffects(B3::PatchpointValue*, OperationEffects);
    void didEmitWrite(Writes, bool writesShapeWords = false, bool convertsArrayStorage = false);
    template<auto operation, typename... Arguments> auto callOperation(Entry, OperationEffects, Arguments...);
    LValue callFunction(FunctionID, LType, const Vector<LValue, 8>& arguments, FunctionID summarizedAs = { }, LValue closure = nullptr, bool isFromDroppableFrame = false);
    LValue callFunctionFromColdBlock(FunctionID, bool returnsValue = false);
    uint32_t calleeIndex(FunctionID);
    bool isLastCallOfGenericEntrypoint(FunctionID) const;
    bool mayJumpTo(FunctionID, const Vector<LValue, 8>& arguments) const;
    Operand lowerCallBySignature(ValueID, const Value&, StringID methodName);
    void callFromCollapsibleFrame(FunctionID, const Vector<LValue, 8>& arguments, LValue closure);
    bool tailCallDropsMachineFrame(const Block&, const Value& call) const;
    std::optional<ValueID> valueReturnedAfter(const Block&) const;
    std::optional<unsigned> returnedParameterOf(FunctionID) const;
    bool canTailCallDirectly(FunctionID, unsigned numberOfGPRs, unsigned numberOfFPRs, const Type& result, bool returnsArgument) const;
    bool canTailCallValue() const { return m_tailCall && !m_dropsDroppableCallerFrame && !m_tailCallReturnsArgument && m_program.at(m_tailCall->type).isAnything() && resultRepOf(m_program, m_id) == Rep::JSValue; }
    Word32 isCalledFromDroppableFrame();
    bool callsFromDroppableFrame(FunctionID) const;
    bool passesPointerIntoFrame(const Value&) const;
    bool mayCallDirectlyFromTailCallSite(FunctionID, const Type& result) const;
    bool mayJumpToWidenedFunction(FunctionID) const;
    bool isWidenedInIndirectTailCallCycle() const;
    bool isLoweringCallInTailPosition() const { return m_currentBlock && m_currentIndexInBlock < m_currentBlock->values.size() && m_function.at(m_currentBlock->values[m_currentIndexInBlock]).isInTailPosition; }
    void checkCallableBeforeTailCall(Boxed& callee, StringID methodName);
    void findTailCallsEmittedAsJumps();
    void didEmitTailCall();
    BitVector m_tailCallsEmittedAsJumps;
    BitVector m_tailCallCandidates;
    BitVector m_tailCallsReturningArgument;
    bool m_hasSelfTailCall { false };
    LBasicBlock m_selfTailCallLoopHeader { nullptr };
    Vector<LValue, 8> m_selfTailCallPhis;
    const Value* m_tailCall { nullptr };
    bool m_tailCallReturnsArgument { false };
    bool m_callIsAwaitedWithoutPromise { false };
    bool m_dropsDroppableCallerFrame { false };
    bool m_leavesDroppableFrame { false };
    bool m_isBeforeTailCall { false };
    void jumpToFunction(uint32_t calleeIndex, const Vector<LValue, 8>& arguments, LValue closure = nullptr, bool dropsCallerFrame = false);
    Operand jumpToContinuation(const Value&);
    Boxed callValue(Stub, const Vector<LValue, 8>& operands, OperationEffects = { }, LValue word = nullptr, SpeculatedMethod = SpeculatedMethod::None, StringID methodName = { }, std::optional<StringID> nameForCallableCheck = std::nullopt);
    Boxed callValueWithArray(Stub, Boxed callee, Boxed thisValue, Boxed list, bool mayTailCall = true);
    Boxed lowerGetNewTarget();
    Boxed callValueWithList(Boxed callee, Boxed thisValue, Word32 count, Pointer list, LValue constructorWord = nullptr);
    FunctionID listEntryFor(FunctionID);
    Boxed callListEntry(FunctionID entry, LValue closure, Boxed thisValue, const Arguments&);
    bool isPassedInMemory(ValueID);
    template<typename T, typename Functor> T lazilyInitialized(Stub, ptrdiff_t table, uint64_t indexAndMore, const Functor& fill);
    enum class FieldInit : uint8_t { ByCaller, FromTemplate };
    CellPointer allocate(uint32_t site) { return allocate(site, integer(site), FieldInit::ByCaller); }
    CellPointer allocate(uint32_t site, Integer number, FieldInit);
    CellPointer allocateObjectCopy(uint32_t site, CellPointer source, unsigned fieldBytes);
    LValue callAllocationStub(Stub, const Vector<std::pair<LValue, GPRReg>, 4>& operands, OperationEffects, bool mayThrow, bool isSlowPathCall = false, bool resultIsDouble = false, LValue number = nullptr, std::optional<uint32_t> thunkImmediate = std::nullopt, Stub stubWithoutThunk = Stub::TypedString, uint32_t operandWithoutThunk = 0, std::span<const LValue> additionalNumbers = { });
    struct SubstringCharacters {
        LValue data;
        LValue flags;
        LValue length;
    };
    SubstringCharacters substringCharacters(CellPointer);
    SubstringCharacters ropeCharacters(CellPointer);
    Integer allocationFor(const Value& newObject);
    std::optional<uint32_t> constantAllocation(const Value& newObject) const;
    bool layoutVariesByDiscriminant(const Allocation&) const;
    CellPointer stringConstant(StringID);
    Boxed keyConstant(StringID);
    bool isLazilyFetched(ValueID id) const { return m_isLazilyFetched.get(id.index()); }
    bool operandsAreAvailable(ValueID id) const
    {
        bool result = true;
        m_function.at(id).forEachChild([&](ValueID child) {
            result &= m_values[child.index()].rep != Rep::Void || isLazilyFetched(child);
        });
        return result;
    }
    bool m_isCompact { false };
    bool isCompact() const { return m_isCompact && m_currentBlock && !m_isInLoop; }
    bool prefersGenericPathInColdCode() const { return !m_isInLoop && !m_program.isHotOutsideLoops(m_id); }
    void poll();
    std::pair<Word32, Pointer> argumentBuffer(const Arguments&);
    LValue transientBuffer(size_t bytes);
    B3::Air::StackSlot* m_transientBuffer { nullptr };
    Vector<B3::Air::StackSlot*, 2> m_stringAppendSlots;
    std::pair<CellPointer, Pointer> allocateArray(Word32 length, Word32 vectorLength, LBasicBlock slowPath);
    CellPointer newArray(const Arguments&);
    CellPointer newArrayFromBuffer(Word32 count, Pointer buffer);
    Word32 restLength(uint32_t numberOfArgumentsToSkip);
    CellPointer createRest(uint32_t numberOfArgumentsToSkip);
    Boxed lowerGetMyArgumentByVal(const Value&);
    Boxed lowerCallForwardVarargs(const Value&);
    Integer entryWordIfClosure(CellPointer);
    std::optional<Pointer> createThisCache();
    CellPointer createThis(CellPointer constructor, CellPointer newTarget, std::optional<Pointer> siteCache = std::nullopt);
    Boxed constructClosure(CellPointer constructor, Integer word, CellPointer newTarget, const Arguments&, std::optional<Pointer> siteCache = std::nullopt, std::optional<uint32_t> numberOfArgumentsToSkip = std::nullopt);
    Boxed constructDerivedClosure(CellPointer constructor, Integer word, Boxed newTarget, const Arguments&);
    Operand lowerConstructSuper(ValueID, const Value&);
    CellPointer lowerCreateThis(const Value&);
    struct Argument {
        Operand operand;
        const Type* type;
    };
    Argument argument(ValueID id) { return { get(id), &typeOf(id) }; }
    Argument argumentFor(const Function& callee, unsigned parameter, ValueID);
    const Type& declaredArgumentType(const Function& callee, unsigned index) const;
    LValue unusedParameterPlaceholder(Rep);
    Operand callCompiled(FunctionID, const Vector<Argument, 8>&, const Type& result, LValue* multiResultCall = nullptr, bool isCallBeingLowered = false);
    struct MethodTarget {
        FunctionID function;
        Vector<uint32_t, 4> shapes;
        const Field* fieldCheckedFirst { nullptr };
    };
    using MethodTargets = Vector<MethodTarget, 4>;
    std::optional<MethodTargets> methodTargets(LayoutID supertype, StringID) const;
    Operand callMethod(FunctionID, CellPointer thisObject, const Arguments&, const Type& result);
    Operand callSubtypeMethod(CellPointer, const Type& receiver, const MethodTargets&, const Value&, const Type& result);
    Boxed callGeneric(Boxed callee, Boxed thisValue, const Arguments&, StringID methodName = { }, SpeculatedMethod = SpeculatedMethod::None, bool isKnownToBeFunction = false);
    Boxed callMethodGeneric(Boxed object, StringID, const Arguments&);
    Boxed genericCallArgument(ValueID);
    Boxed copyClosureToHeap(Boxed);
    ValueID m_argumentCopiedToHeap;
    Boxed m_heapCopy;
    Boxed callMethodThroughSlot(Boxed object, unsigned slot, const Arguments&, SpeculatedMethod = SpeculatedMethod::None, bool slotIsNameIndex = false);
    Boxed tailCallMethodByName(Boxed object, unsigned nameIndex, const Arguments&);
    std::optional<uint32_t> tableIndexOfName(StringID);
    bool canCallInsteadOfClosure(FunctionID, unsigned numberOfArguments) const;
    Operand callClosure(FunctionID, CellPointer closure, ValueID thisValue, const Arguments&, const Type& result, LBasicBlock otherwise);
    Operand lowerCallDirect(ValueID result, const Value&);
    bool isModuleEvaluatedBefore(ValueID evaluation, uint32_t row);
    std::optional<Operand> readHostObjectMember(const Value&);
    std::optional<Operand> readHostObjectMemberInline(ValueID result, const Value&);
    std::optional<Operand> readAsyncLocalStorage(const Value&);
    Vector<B3::ValueRep, 4> resultRegisters(const Function&) const;
    void returnResults(const Value&);
    Operand lowerResultAt(const Value&);
    UncheckedKeyHashMap<uint32_t, LValue, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_multiResultCalls;
    bool isGuardedCallee(ValueID);
    Operand lowerIndirectCall(ValueID callee, ValueID thisValue, const Arguments&, const Type& result, StringID methodName = { });
    Word32 lowerIsBuiltinFunction(const Value&);
    const Function* definingFunctionOf(ValueID) const;
    Boxed getCallOrApply(Boxed base, StringID, BuiltinID, const Function* definedAs);
    std::optional<Operand> lowerCallOrApply(ValueID result, ValueID function, StringID, std::optional<Boxed> method, const Arguments&);
    Operand lowerApply(ValueID callee, ValueID thisValue, ValueID list, const Type& result, LanguageOperation = LanguageOperation::Apply);
    Operand lowerCall(ValueID result, const Value&);
    Operand lowerCallMethod(ValueID result, const Value&);
    Operand lowerConstruct(ValueID result, const Value&);
    Operand lowerConstructWith(ValueID result, const Value&);
    FunctionID genericEntrypointFor(FunctionID);
    Operand closureCapture(CellPointer, const Function&, unsigned index);
    LValue loadRuntimeState32(ptrdiff_t offsetInInstance) { return m_out.load32(m_out.address(m_heaps.AOTTyped_runtimeState, m_instance, offsetInInstance)); }
    CellPointer closureConstant(FunctionID);
    CellPointer lowerNewClosure(const Value&, LValue cachedClosureVariable = nullptr, Store variableStore = Store::JSValue, bool* isStoredByStub = nullptr);
    CellPointer lowerNewClosureOnce(const Value&);
    bool isResumeClosureCreatedAtSuspend(size_t index);
    bool isResumeClosureCreatedByStub(size_t index);

    LValue loadICData();
    std::optional<unsigned> allocateSite(StringID name, unsigned extra);
    struct AccessRun {
        B3::Variable* verdict { nullptr };
        unsigned header { 0 };
        uint32_t stored { 0 };
        Vector<StringID, 8> names;
        uint32_t findStructureDescriptor { Format::none };
        Vector<uint8_t> places;
    };
    void findStructureDescriptorOfRun(AccessRun&) const;
    void findAccessRuns(const Vector<BlockID>& order);
    void didDefineAccessRunReceiver(ValueID);
    void invalidateAccessRunVerdicts();
    const AccessRun* currentAccessRun(StringID);
    LValue slotFromAccessRun(const AccessRun&, Boxed base, StringID, LBasicBlock own);
    Boxed emitStandaloneCachedGetById(Boxed base, StringID, bool isInRun = false);
    bool usesSiteCaches(StringID name = { }) const;
    bool receiverMayBeHostObject(StringID name) const;
    Boxed getByNameThroughSiteCache(Boxed base, uint32_t name);
    void putByNameThroughSiteCache(bool isSloppy, Boxed base, uint32_t name, Boxed value);
    void emitPutByNameThroughTable(Stub, Boxed base, uint32_t identifier, Boxed value);
    std::optional<Stub> getByNameStubForType(const Type&) const;
    std::optional<Operand> getByIdWithTypeCheck(Boxed base, StringID, TypeID held, std::optional<size_t> castIndex = std::nullopt);
    Operand getByIdAndCheckType(Boxed base, StringID, const Type&, const Function* definedAs = nullptr);
    std::optional<Operand> getByNameForFollowingCast(ValueID result, const Value&);
    bool isPlainReadByName(size_t index);
    std::optional<Operand> getByNameWithFollowingRead(ValueID result, const Value&);
    std::optional<Operand> getByNameForFollowingBranch(ValueID result, const Value&);
    void branchOnTruthinessOfRead(const Value& read, FTL::WeightedTarget isTruthy, FTL::WeightedTarget isFalsy);
    std::optional<Operand> getByNameForFollowingComparison(ValueID result, const Value&);
    std::optional<Operand> getByNameForFollowingCall(ValueID result, const Value&);
    void branchOnLiteralOfRead(const Value& read, std::span<const uint8_t> text, LBasicBlock isText, LBasicBlock isOther);
    enum class SlotAddressUse : bool { FastPath, ColdBlock };
    Pointer slotAddress(unsigned slot, SlotAddressUse = SlotAddressUse::FastPath);
    LValue callICStub(Stub, LType, const Vector<std::pair<LValue, GPRReg>, 4>& operands, std::optional<std::pair<GPRReg, uint32_t>> immediate = std::nullopt, OperationEffects = { }, std::optional<std::pair<unsigned, GPRReg>> slot = std::nullopt, LValue number = nullptr);
    Boxed emitGetById(Boxed base, StringID, bool isCalled = false, const Function* definedAs = nullptr);
    Boxed emitCachedGetById(Boxed base, StringID, bool isCalled = false);
    CellPointer newEmptyObject(unsigned numberOfNames);
    UncheckedKeyHashMap<uint32_t, unsigned, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_completeAllocationSlots;
    TypedPointer knownSlotIn(CellPointer, const Value&);
    CellPointer receiverAtKnownSlot(const Value&, Boxed base);
    LValue checkedKnownSlot(const Value&, Boxed base);
    void reportUnlessAtKnownSlot(LValue found, LValue expected, Boxed base);
    Boxed getAtKnownSlot(const Value&, Boxed base);
    void putAtKnownSlot(const Value&, Boxed base, Boxed value);
    void assignThisStructure(const Value& store, Boxed self);
    bool hasInitialStructureStill(const Value& store);
    Vector<uint32_t> m_singlePredecessors;
    UncheckedKeyHashMap<uint32_t, bool> m_isUnreachableFromHandler;
    Boxed emitGetLength(Boxed base);
    Boxed emitGetLengthByStub(Boxed base);
    bool isReadByIndexInLoop(ValueID);
    std::optional<BitVector> m_valuesReadByIndexInLoops;
    bool isInSloppyCode() const;
    void setByName(Boxed base, StringID, Boxed value);
    void emitPutById(Boxed base, StringID, Boxed value);
    bool m_namedStoreValueIsNotCell { false };
    Boxed emitGetByVal(Boxed base, Boxed key, bool mayBeName);
    void emitPutByVal(Boxed base, Boxed key, Boxed value, bool mayBeName);
    std::optional<Word32> emitDeleteByVal(Boxed base, Boxed key);
    Boxed emitGenericArithmetic(Stub, Boxed, Boxed, OperationEffects);
    Word32 emitGenericCompare(Stub, Boxed, Boxed, OperationEffects);

    struct Assumption {
        ValueID value;
        Operand operand;
        const Type* type;
    };
    struct Expectation {
        ValueID value;
        const Type* type;
        bool hasOriginalStructure { false };
        bool rejectsUntypedObjects { false };
    };
    enum class PredictionUse : bool { ChoosesFasterPath, DecidesLayoutAtBirth };
    std::optional<Type> predictionOf(ValueID, uint16_t without = Primitive::UndefinedOrNull, PredictionUse = PredictionUse::ChoosesFasterPath) const;
    Type predictedTypeOf(ValueID) const;
    Operand speculateOrElse(const Expectation&, LBasicBlock otherwise);
    template<typename InlinePath, typename GenericPath> Operand ifPredictionHolds(const Vector<Expectation, 4>&, Rep, const InlinePath&, const GenericPath&, LBasicBlock sharedSlowPath = nullptr);
    bool arePredictedNumbers(std::initializer_list<ValueID>) const;
    template<typename OnInt32, typename OnNumbers, typename GenericPath> Operand ifPredictedNumbers(std::initializer_list<ValueID>, Rep, bool hasCodeForInt32, const OnInt32&, const OnNumbers&, const GenericPath&, bool acceptsAnyInteger = false, std::optional<std::pair<LBasicBlock, LBasicBlock>> successors = std::nullopt);

    Integer indexOrElse(ValueID key, LBasicBlock otherwise, bool boundsCheckRejectsNegatives = false);
    std::optional<IntegralNumberAnalysis> m_integralNumbers;
    std::optional<Operand> absentElement(const Type&);
    Boxed genericGetByVal(ValueID object, ValueID key);
    void genericPutByVal(ValueID object, ValueID key, ValueID stored);
    Pointer butterflyOf(CellPointer);
    Integer publicLength(Pointer butterfly);
    Integer vectorLength(Pointer butterfly);
    Integer arrayLength(CellPointer);
    Word32 isPlainArray(CellPointer array);
    Word32 isModifiedArray(CellPointer);
    Word32 indexingModeOf(CellPointer array, unsigned mask);
    Word32 anyArrayIndexingMode(CellPointer array, unsigned mask);
    bool mayBeIndex(ValueID key) const;
    bool isPredictedNumber(ValueID element);
    bool isOnlyComparedWithNumbers(ValueID element);
    Number numberOrNaN(Boxed);
    Number arrayElementAsNumberOrNaN(CellPointer array, Integer index);
    bool isPredictedNonPrimitive(ValueID element);
    bool isOnlyArithmeticOperand(ValueID element);
    Operand getArrayElement(CellPointer array, ValueID key, const Type&, LBasicBlock otherwise = nullptr, bool expectsNumbers = true);
    void findNumberArraysUsedInLoops();
    void convertNumberArraysBeforeLoops(const Terminator&, bool isAtBlockStart);
    void convertInt32sToDoubles(ValueID array);
    bool isConvertedBeforeLoop(ValueID array) const;
    bool m_arrayIsConvertedBeforeLoop { false };
    bool m_arrayIsAllocatedAsDouble { false };
    Vector<Vector<ValueID, 2>> m_numberArraysConvertedBeforeLoops;
    Word32 isBeyondBarrierThreshold(CellPointer owner);
    template<typename Functor> void storeBoxedArrayElement(CellPointer array, Integer index, Boxed stored, LBasicBlock otherwise, const Functor& afterwards);
    template<typename Functor> void storeArrayElement(CellPointer array, Integer index, ValueID stored, LBasicBlock otherwise, const Functor& afterwards);
    void putArrayElement(CellPointer array, ValueID key, ValueID stored, LBasicBlock otherwise = nullptr, ValueID receiver = { });
    Word32 tryStoreArrayElement(CellPointer array, Integer index, ValueID stored);
    Operand pushArrayElement(CellPointer array, ValueID receiver, ValueID pushed, const Type&);
    Operand pushArrayElements(CellPointer array, const Value&, const Type&);
    void ensureStillAliveHere(CellPointer owner, const char* file = __builtin_FILE(), unsigned line = __builtin_LINE());
    void ensureStillAliveHereForWord(CellPointer owner, LValue word, const char* file = __builtin_FILE(), unsigned line = __builtin_LINE());
    static bool isDroppedForTesting(const char* file = __builtin_FILE(), unsigned line = __builtin_LINE());
    static bool isForcedForTesting(const char* file, unsigned line);
    struct TypedArrayReceiver {
        CellPointer view;
        unsigned flags { 0 };
        Word32 isView;
        Boxed value;
        bool isHoisted { false };
        Word32 isFixedLengthView { };
        Integer length { };
        Pointer vector { };
    };
    struct HoistedTypedArray {
        ValueID array;
        B3::Variable* view { nullptr };
        B3::Variable* isFixedLengthView { nullptr };
        B3::Variable* length { nullptr };
        B3::Variable* vector { nullptr };
    };
    Vector<Vector<HoistedTypedArray, 1>> m_typedArraysHoistedBeforeLoops;
    BitVector m_accessesOfHoistedTypedArrays;
    void findTypedArraysAccessedInLoops();
    bool mayChangeTypedArrayFields(const Loop&) const;
    void hoistTypedArrayFieldsBeforeLoops(const Terminator&);
    struct CalleeResolvedBeforeLoop {
        ValueID callee;
        unsigned numberOfArguments;
        B3::Variable* target;
    };
    struct ResolvedCall {
        LValue callee;
        LValue target;
        unsigned numberOfArguments;
    };
    Vector<Vector<CalleeResolvedBeforeLoop, 1>> m_calleesResolvedBeforeLoops;
    UncheckedKeyHashMap<uint32_t, CalleeResolvedBeforeLoop, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_callsOfResolvedCallees;
    std::optional<ResolvedCall> m_resolvedCall;
    void findCalleesCalledInLoops();
    void resolveCalleesBeforeLoops(const Terminator&);
    std::optional<ResolvedCall> resolvedCallAt(ValueID);
    UncheckedKeyHashMap<uint32_t, SpeculatedMethod, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_methodsReadAheadOfArguments;
    UncheckedKeyHashMap<uint32_t, SpeculatedMethod, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_callsOfMethodsReadAheadOfArguments;
    SpeculatedMethod m_methodBeingReadAheadOfArguments { SpeculatedMethod::None };
    void findMethodsReadAheadOfArguments();
    TypedArrayReceiver typedArrayElementReceiver(ValueID, bool isBeforeLoop = false);
    TypedArrayReceiver typedArrayReceiver(ValueID);
    TypedArrayReceiver typedArrayReceiver(CellPointer);
    void rejectNullish(const TypedArrayReceiver&);
    Word32 hasFixedLength(const TypedArrayReceiver&);
    Pointer findTypedArrayElement(const TypedArrayReceiver&, Integer index, LBasicBlock otherwise, LBasicBlock outside);
    Operand getTypedArrayElement(const TypedArrayReceiver&, ValueID key, TypedArrayType, const Type&, LBasicBlock otherwise = nullptr);
    void putTypedArrayElement(const TypedArrayReceiver&, ValueID key, TypedArrayType, ValueID stored, LBasicBlock otherwise = nullptr);
    Operand lowerGetByVal(ValueID result, const Value&);
    void lowerPutByVal(const Value&);
    Word32 isArrayUsedAsIterator(ValueID);
    Word32 isArrayUsedAsIteratorBehindStep(ValueID);
    struct IteratorKindBehindStep {
        LValue kind { nullptr };
        BlockID block;
    };
    UncheckedKeyHashMap<uint32_t, IteratorKindBehindStep, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_iteratorKindsBehindSteps;
    bool arraysMayChangeDuringIteration(ValueID iterator) const;
    Operand lowerIteratorOpen(ValueID result, const Value&);
    Word32 lowerIteratorHasNext(const Value&, bool isUnconditional = true);
    Operand lowerIteratorValue(ValueID result, const Value&);
    void lowerIteratorClose(const Value&);

    struct StringReceiver {
        CellPointer string;
        unsigned flags { 0 };
        Word32 isString;
        Boxed value;
        bool isCellOrNullish { false };
        ValueID id { };
    };
    struct Characters {
        Pointer data;
        Word32 flags;
        Integer length;
        CellPointer owner;
    };
    struct StringPart {
        CellPointer string;
        Integer length;
        Word32 flags;
        bool mayBeEmpty { true };
    };
    StringReceiver stringReceiver(ValueID);
    StringReceiver stringReceiver(CellPointer);
    void rejectNullish(const StringReceiver&, StringID name);
    Pointer fiberOf(const StringReceiver&);
    Integer stringLength(const StringReceiver&);
    Characters charactersOf(const StringReceiver&, LBasicBlock ifRope = nullptr);
    Word32 codeUnitAt(const Characters&, Integer index);
    Word32 codePointAt(const Characters&, Integer index);
    CellPointer stringFromCodeUnit(Word32);
    CellPointer singleCharacterStringAt(const Characters&, Integer index, Word32* codeUnit = nullptr);
    Operand getStringElement(const StringReceiver&, ValueID key, const Type&, LBasicBlock otherwise = nullptr, B3::Variable* codeUnitVariable = nullptr);
    Integer positionOf(ValueID);
    Integer relativeStringIndex(ValueID, Integer length, bool isRelativeToEnd);
    Integer relativeStringIndex(Operand, const Type&, Integer length, bool isRelativeToEnd);
    Operand numberOrIfUndefined(ValueID, Number otherwise);
    CellPointer allocateRope(LBasicBlock slowPath);
    CellPointer makeSubstring(const StringReceiver&, Integer length, Integer from, Integer to);
    std::optional<std::span<const uint8_t>> constantASCIIString(ValueID) const;
    std::optional<uint32_t> nonNumericStringConstant(ValueID) const;
    StringPart stringPartOf(ValueID);
    StringPart stringPartOf(CellPointer);
    StringPart concatenate(const StringPart&, const StringPart&, const StringPart* third = nullptr);
    CellPointer lowerStrcat(const Value&);
    CellPointer integerToString(Integer, const Type&);
    Word32 hasText(Pointer data, std::span<const uint8_t> text, bool leavesAtFirstDifference = false);
    Word32 textMatches(Pointer data, std::span<const uint8_t> text) { return hasText(data, text, true); }
    Word32 areBytesEqual(Pointer, Pointer, Integer length);
    Word32 hasSubstringAt(const StringReceiver&, Integer place, ValueID constant, std::span<const uint8_t> text);
    Word32 isStringEqualTo(ValueID, ValueID constant, std::span<const uint8_t> text);
    Word32 areStringsEqual(ValueID, ValueID);
    Boxed mapKey(ValueID);
    Word32 mapKeyHash(Boxed);
    Pointer findInMapOrSet(CellPointer table, bool isMap, Boxed key, Word32 hash, LBasicBlock absent);
    Integer mapOrSetSize(CellPointer, bool isMap, bool hasStringKeysByType);
    bool tableStubChecksReceiver(ValueID result, const Value&);
    Operand lowerMapOrSetBuiltin(InlineBuiltin, ValueID result, const Value&, Writes inlinePath);
    bool checksMapGetResultAfterJoin(const Value&);

    CellPointer builtinCell(unsigned);
    Word32 hasOriginalStructure(CellPointer, const EngineClass&);
    void validateReceiverHasOriginalStructure(Word32 holds, CellPointer, ValueID);
    Boxed callBuiltinFunction(const Value&, OperationEffects, std::optional<Boxed> calledOn = std::nullopt);
    bool createsStringKeyedTable(const Value&);
    bool m_isCreatingTableGenerically { false };
    bool m_isApplyingGenerically { false };
    bool m_isFreezingGenerically { false };
    bool m_isCreatingArrayGenerically { false };
    Operand lowerGenericFallback(const Value&, CellPointer receiver, const Type&, bool methodWasFound = false);
    bool needsOnlyMethod(const Value&) const;
    Boxed lowerMethodGetById(const Value&);
    Operand lowerCallBuiltin(ValueID result, const Value&);
    Operand lowerResolvedBuiltin(ValueID result, const Value&);
    Operand lowerIndexIntrinsic(InlineBuiltin, const Value&, const Type&);
    struct ElementFound {
        LValue object { nullptr };
        LValue key { nullptr };
        LValue element { nullptr };
        bool isAlwaysPresent { false };
    };
    bool isInLoopOfOriginalFunction(const Value&) const;
    ValueID getIndexAfterBranchOn(const Value& hasIndex);
    UncheckedKeyHashMap<uint32_t, ElementFound, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_elementsFoundByHasIndex;
    Operand numberResult(Number, const Type&);
    Operand booleanResult(Word32, const Type&);
    Operand valueResult(ValueID, const Type&);
#define AOT_TYPED_DECLARE_LOWER_FAMILY(family) std::optional<Operand> lowerInline ## family(InlineBuiltin, ValueID result, const Value&, Writes inlinePath);
    FOR_EACH_AOT_TYPED_BUILTIN_FAMILY(AOT_TYPED_DECLARE_LOWER_FAMILY)
#undef AOT_TYPED_DECLARE_LOWER_FAMILY
#include "AOTTypedBuiltinMembers.inc"
    std::optional<Operand> lowerInline(InlineBuiltin, ValueID result, const Value&);
    Operand lowerContainerBuiltin(InlineBuiltin, ValueID result, const Value&, Writes inlinePath);
    struct PredictedBuiltin {
        Value call;
        Type receiver;
        Vector<Type, 4> arguments;
        bool isFunctionCalledAsValue { false };
    };
    BuiltinName globalNameOf(ValueID) const;
    std::optional<PredictedBuiltin> predictedBuiltin(BuiltinKind, ValueID receiver, BuiltinName, const Arguments&) const;
    std::optional<PredictedBuiltin> predictedBuiltin(BuiltinKind, ValueID receiver, StringID, const Arguments&) const;
    bool choosePredictedArguments(PredictedBuiltin&) const;
    std::optional<Operand> lowerBuiltinFunctionCall(ValueID result, ValueID callee, ValueID thisValue, const Arguments&);
    template<typename GenericPath> Operand lowerPredictedBuiltin(ValueID result, const PredictedBuiltin&, std::optional<Boxed> callee, const GenericPath&);

    const Program& m_program;
    const Function& m_function;
    FunctionID m_id;
    bool m_isEntry;
    LoweringContext& m_context;
    const Builtins& m_builtins;
    Vector<Operand> m_values;
    Vector<Assumption, 4> m_assumptions;
    struct PreloadedElement {
        ValueID array;
        ValueID index;
        LValue element { nullptr };
    };
    UncheckedKeyHashMap<uint32_t, PreloadedElement, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_preloadedElements;
    LValue preloadedElement(ValueID array, ValueID index);
    bool m_isOnConditionalPath { false };
    bool m_defersNumbersToLoadElement { false };
    bool m_usesElementStub { false };
    bool m_queriesExtraPropertiesForAbsentField { false };
    LValue m_valueCheckedByColdOperation { nullptr };
    const ArgumentMoves* m_coldOperationTypeTests { nullptr };
    bool m_mapGetReturnsAnything { false };
    bool isOnMainPath() const { return !m_isOnConditionalPath; }
    struct FusedCasts {
        size_t readIndex;
        unsigned count;
        Type type;
        bool areBeingLowered { false };
        std::optional<size_t> useIndex { };
        unsigned countAfterUse { 0 };
    };
    std::optional<FusedCasts> m_fusedCasts;
    struct CastFusedIntoRead {
        const Block* block;
        size_t index;
        Operand operand;
    };
    std::optional<CastFusedIntoRead> m_castFusedIntoRead;
    std::optional<CastFusedIntoRead> m_readFusedIntoRead;
    ValueID m_readDeferredToBranch;
    ValueID m_readDeferredToComparison;
    ValueID m_readDeferredToCall;
    size_t m_indexOfReadDeferredToComparison { 0 };
    UncheckedKeyHashMap<uint32_t, B3::Variable*, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_elementsKnownPresent;
    bool m_indexIsKnownInBounds { false };
    bool m_needsPoll { false };
    Vector<uint32_t> m_numberOfUses;
    Vector<uint32_t> m_firstUser;
    Vector<ValueID> m_users;
    BitVector m_isUsedByTerminator;
    Vector<std::pair<uint32_t, uint32_t>> m_valueLocations;
    UncheckedKeyHashMap<uint32_t, PinnedRead, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_pinnedReads;
    UncheckedKeyHashMap<uint32_t, LValue, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_elementsReadAsNumbers;
    Vector<LBasicBlock> m_blocks;
    Vector<Vector<LValue, 4>> m_phis;
    BitVector m_hasTruthinessPhi;
    BitVector m_isLazilyFetched;
    BitVector m_isFusedWithBranch;
    BitVector m_isTruthinessOnlyBranchedOn;
    ValueID m_truthinessDeferredToBranch;
    std::optional<std::pair<FTL::WeightedTarget, FTL::WeightedTarget>> m_truthinessSuccessors;
    std::optional<std::pair<LBasicBlock, LBasicBlock>> m_relationalSuccessors;
    BitVector m_hasNonAwaitUse;
    bool m_didFindNonAwaitUses { false };
    struct CheckedReceiver {
        ValueID receiver;
        uint32_t engineClass { 0 };
        bool isOriginal { false };
    };
    UncheckedKeyHashMap<uint32_t, CheckedReceiver, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_checkedReceivers;
    UncheckedKeyHashMap<const Block*, std::pair<ValueID, LValue>> m_inlineAwaitedValues;
    struct Truthiness {
        LValue value { nullptr };
        LValue isTruthy { nullptr };
    };
    UncheckedKeyHashMap<uint32_t, Truthiness, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_truthinessPhis;
    BitVector m_isSplitJoin;
    UncheckedKeyHashMap<uint32_t, LValue, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_cellPhis;
    bool m_isLoweringSunkCoercion { false };
    UncheckedKeyHashMap<LValue, LValue> m_asDouble;
    UncheckedKeyHashMap<LValue, LValue> m_boxedBeforeUnboxing;
    UncheckedKeyHashSet<LValue> m_unusedParameterPlaceholders;
    UncheckedKeyHashMap<LValue, TypeID> m_cellTypes;
    UncheckedKeyHashSet<LValue> m_assumedCells;
    struct Decoded {
        LValue index;
        LValue cell;
        LValue loaded;
        bool absentReadsUndefined;
    };
    UncheckedKeyHashMap<LValue, Decoded> m_decoded;
    UncheckedKeyHashMap<LValue, LValue> m_beforeAbsentNormalization;
    Operand beforeAbsentNormalization(Operand operand) const
    {
        auto found = m_beforeAbsentNormalization.find(operand.value);
        return found == m_beforeAbsentNormalization.end() ? operand : Operand(operand.rep, found->value);
    }
    UncheckedKeyHashMap<LValue, LValue> m_cellIndices;
    StringID m_length;
    UncheckedKeyHashMap<uint32_t, AccessRun, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_accessRuns;
    UncheckedKeyHashSet<uint64_t> m_followingAccesses;
    UncheckedKeyHashSet<uint64_t> m_dominatedAccesses;
    UncheckedKeyHashSet<B3::Value*> m_structurePreservingStores;
    static uint64_t accessRunKey(BlockID block, unsigned index) { return (static_cast<uint64_t>(block.index()) << 32 | index) + 1; }
    BitVector m_loopBlocks;
    OpcodeSet m_opcodes;
    BitVector m_handlersEnteredByBranch;
    const Target* m_activeHandler { nullptr };
    LBasicBlock m_currentLandingPad { nullptr };
    bool m_isCurrentLandingPadShared { false };
    UncheckedKeyHashMap<uint32_t, LValue, WTF::IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_caughtValues;
    UncheckedKeyHashMap<uint32_t, LBasicBlock, WTF::IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_catchBlocks;
    struct LandingPad {
        BlockID source;
        const Target* handler;
        LBasicBlock block;
    };
    UncheckedKeyHashMap<uint32_t, Vector<LandingPad, 2>, WTF::IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_landingPads;
    unsigned m_callSitesAwaitingLandingPad { 0 };
    BuiltinID m_predictedMethodAlreadyRead;
    LBasicBlock m_sharedGenericPath { nullptr };
    BuiltinID m_sharedGenericPathBuiltin;
    LBasicBlock newBlockForGenericCall(const Value& call) { return m_sharedGenericPath && call.builtin() == m_sharedGenericPathBuiltin ? m_sharedGenericPath : newColdBlock(); }
    bool isSharedGenericPath(LBasicBlock block) const { return block == m_sharedGenericPath; }
    mutable UncheckedKeyHashMap<uint32_t, uint32_t, WTF::IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_minObjectSizes;
    mutable UncheckedKeyHashMap<uint32_t, bool, WTF::IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_mayBeFrozenCache;
    mutable UncheckedKeyHashMap<uint32_t, bool, WTF::IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_mayDespecializeCache;
    bool m_didFindStoresToVariables { false };
    BitVector m_storedVariables;
    UncheckedKeyHashMap<uint32_t, Vector<BlockID, 2>, WTF::IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_blocksCheckingInitialization;
    UncheckedKeyHashSet<uint64_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_storedFields;
    bool m_didFindGuardedCallees { false };
    BitVector m_guardedCallees;
    BitVector m_moduleEvaluatingBlocks;
    UncheckedKeyHashMap<uint32_t, FunctionID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_listEntries;
    bool m_didFindRestArraysPassedInMemory { false };
    BitVector m_restArraysPassedInMemory;
    UncheckedKeyHashMap<uint32_t, FunctionID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_listEntryByCall;
    bool m_isStoringToLocalAllocation { false };
    Vector<uint8_t> m_integerComparisonCounts;
    bool m_didFindInt32Uses { false };
    BitVector m_isUsedAsInt32;
    UncheckedKeyHashMap<LValue, LValue> m_numberToInt32;
    UncheckedKeyHashMap<LValue, LValue> m_numberToExactInteger;
    UncheckedKeyHashMap<LValue, LValue> m_numberToWord;
    bool m_fieldIsPresent { false };
    const ScopedLambda<void()>* m_absentSlotHandler { nullptr };
    bool m_checksUntypedObjectResult { false };
    bool m_checksExtraPropertyResult { false };
    struct NumberAsInteger {
        LValue number;
        LValue integer;
        std::optional<int64_t> minimum { };
    };
    UncheckedKeyHashMap<uint32_t, NumberAsInteger, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_numbersAsIntegers;
    FirstRepeatedComparisonMap m_firstRepeatedComparisons;
    FirstRepeatedComparisonMap m_firstRepeatedShortStringComparisons;
    UncheckedKeyHashMap<uint32_t, std::pair<LValue, LValue>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_cachedShortStrings;
    UncheckedKeyHashMap<uint32_t, std::pair<LValue, LValue>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_cachedComparisonIntegers;
    UncheckedKeyHashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_firstInstanceOfInRun;
    UncheckedKeyHashMap<uint32_t, std::pair<LValue, LValue>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_instanceOfRunResults;
    struct IntegerConvertedToNumber {
        LValue number { nullptr };
        LValue integer { nullptr };
        int64_t minimum { 0 };
    };
    IntegerConvertedToNumber m_lastIntegerConvertedToNumber;
    bool m_tableStubChecksReceiver { false };
    UncheckedKeyHashMap<uint32_t, IntegerConvertedToNumber, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_likelyIntegerNumbers;
    bool m_isLoweringInitialStoresOneByOne { false };
    bool m_rejectsUntypedObjects { false };
    bool m_isCheckingTypeRelaxedForLogging { false };
    std::optional<LoweringContext::SitePosition> m_throwPosition;
    FunctionID m_parameterCheckCallee;
    bool m_isElementOfEntries { false };
    bool m_useCastTest { false };
    bool m_isValidatingFact { false };
    const Block* m_currentBlock { nullptr };
    mutable const Block* m_unescapedAllocationBlock { nullptr };
    mutable ValueID m_unescapedAllocation;
    mutable size_t m_unescapedUpToIndex { 0 };
    struct CollectionCheckCache {
        const Block* block { nullptr };
        size_t index { 0 };
        bool collectorMayHaveRun { false };
    };
    mutable UncheckedKeyHashMap<uint32_t, CollectionCheckCache, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_collectionCheckCache;
    UncheckedKeyHashMap<uint32_t, std::pair<LBasicBlock, Operand>, WTF::IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_fetchedLeaves;
    const Block* m_objectLiteralsBlock { nullptr };
    Vector<ObjectLiteral, 1> m_objectLiterals;
    UncheckedKeyHashMap<size_t, unsigned, DefaultHash<size_t>, WTF::UnsignedWithZeroKeyHashTraits<size_t>> m_objectLiteralAtIndex;
    bool m_objectLiteralIndexIsComplete { false };
    const Block* m_namedStoresBlock { nullptr };
    Vector<NamedStores, 1> m_namedStores;
    const Block* m_storesAfterSingleTestBlock { nullptr };
    Vector<StoresAfterSingleTest, 1> m_storesAfterSingleTest;
    bool m_orderWordHasNoFlag { false };
    const Block* m_accessorGroupsBlock { nullptr };
    Vector<AccessorGroup, 1> m_accessorGroups;
    const Block* m_spreadLiteralsBlock { nullptr };
    Vector<SpreadLiteral, 1> m_spreadLiterals;
    const Block* m_propertyRunsBlock { nullptr };
    Vector<PropertyRun, 1> m_propertyRuns;
    BlockID m_currentBlockID;
    size_t m_currentIndexInBlock { 0 };
    LBasicBlock m_unconditionalBlock { nullptr };
    Vector<unsigned> m_dominatorTreeEnter;
    Vector<unsigned> m_dominatorTreeLeave;
    struct Loop {
        BlockID header;
        BitVector blocks;
    };
    static constexpr unsigned noLoop = UINT_MAX;
    Vector<Loop> m_loops;
    Vector<unsigned> m_innermostLoop;
    Vector<BlockID> m_definingBlock;
    struct BlockAfterNullishTest {
        unsigned enter;
        unsigned maxLeave;
    };
    UncheckedKeyHashMap<uint32_t, Vector<BlockAfterNullishTest, 1>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_blocksAfterNullishTest;
    UncheckedKeyHashMap<uint32_t, Vector<BlockID, 1>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_blocksWhereReadIsDefined;
    UncheckedKeyHashMap<uint32_t, std::pair<LBasicBlock, Operand>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_decodedReads;
    ValueID m_readKnownDefinedOnEdge;
    UncheckedKeyHashMap<LValue, LValue> m_shapeWordCells;
    LValue m_knownTypeCell { nullptr };
    const Type* m_knownCellType { nullptr };
    Vector<uint32_t> m_knownTypeShapes;
    Vector<LayoutID, 1> m_knownTypeShapeLayouts;
    UncheckedKeyHashMap<uint32_t, ValueID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_knownNullishIn;
    mutable UncheckedKeyHashMap<uint32_t, std::unique_ptr<Type>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_typesWithoutNullish;
    UncheckedKeyHashMap<uint32_t, std::unique_ptr<Type>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_widenedLiteralStringTypes;
    Word32 m_argumentCount;
    Pointer m_argumentBuffer;
    bool m_isInLoop { false };
    struct ModuleEvaluationSite {
        ValueID call;
        uint32_t row;
        BlockID block;
        unsigned index;
    };
    std::optional<Vector<ModuleEvaluationSite>> m_moduleEvaluationSites;
    bool m_hasFailedInt32Test { false };
    bool m_retestsOperands { false };
    uint32_t m_nextSite { 1 };
    uint32_t m_lastSite { 0 };
    ASCIILiteral m_failureReason;
    bool m_usesUndefinedValue { false };
    uint8_t m_genericEntrypointParameterCount { 0 };
};

class Lowering::OperandJoin {
public:
    OperandJoin(Lowering& lowering, JoinRep joined)
        : m_lowering(lowering)
        , m_joined(joined)
        , m_continuation(lowering.m_out.newBlock())
    {
    }

    void add(Operand operand)
    {
        RELEASE_ASSERT(operand.rep == m_joined.rep);
        m_incomingValues.append({ m_lowering.m_out.m_block, operand });
    }

    Operand finish();

private:
    struct Arm {
        LBasicBlock block;
        Operand operand;
    };

    Lowering& m_lowering;
    JoinRep m_joined;
    LBasicBlock m_continuation;
    Vector<Arm, 4> m_incomingValues;
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
Operand Lowering::eitherOperand(Word32 condition, JoinRep joined, const Fast& fast, const Slow& slow)
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
Operand Lowering::dispatchOnShape(CellPointer cell, const ShapeTest& test, JoinRep joined, const Passes& onPass, const Fails& onFail)
{
    if (test.neverPasses())
        return onFail();
    LBasicBlock passing = m_out.newBlock();
    LBasicBlock failing = newUnlikelyBlock();
    OperandJoin join(*this, joined);
    m_out.branch(passes(shapeWordOf(cell), test, failing).value(), unsure(passing), unsure(failing));
    m_out.appendTo(passing);
    join.add(onPass());
    m_out.appendTo(failing);
    join.add(onFail());
    return join.finish();
}

template<typename InOwnField, typename Otherwise>
Operand Lowering::dispatchOnSubtypeField(CellPointer cell, LayoutID supertype, const Field& field, JoinRep joined, const InOwnField& inOwnField, const Otherwise& otherwise)
{
    Vector<std::pair<LayoutID, const Field*>, maxSubtypesWithOwnFieldPath> subtypes;
    for (LayoutID subtype : m_program.at(supertype).structuralSubtypes) {
        auto& layout = m_program.at(subtype);
        auto found = layout.fieldNamed(field.name);
        if (!found || layout.fields[*found].slot == field.slot || !layout.fields[*found].slot)
            continue;
        if (subtypes.size() == maxSubtypesWithOwnFieldPath)
            return otherwise();
        subtypes.append({ subtype, &layout.fields[*found] });
    }
    OperandJoin join(*this, joined);
    Word32 shapeWord = shapeWordOf(cell);
    for (auto& [subtype, own] : subtypes) {
        LBasicBlock isThisSubtype = m_out.newBlock();
        LBasicBlock isOtherSubtype = m_out.newBlock();
        m_out.branch(equal(shapeWord, word32(Typed::shapeWordOf(m_program.at(subtype).shape))).value(), unsure(isThisSubtype), unsure(isOtherSubtype));
        m_out.appendTo(isThisSubtype);
        join.add(inOwnField(subtype, *own));
        m_out.appendTo(isOtherSubtype);
    }
    join.add(otherwise());
    return join.finish();
}

template<typename Has, typename Lacks>
Operand Lowering::accessThroughSupertype(CellPointer cell, ValueID object, LayoutID supertype, const Field& field, JoinRep joined, const Has& has, const Lacks& lacks)
{
    if (field.isMissingInSomeSubtype) {
        RELEASE_ASSERT(!field.subtypesWithField.neverPasses() || field.hasOtherOffsetInSomeSubtype || !hasSubtypeWithField(supertype, field.name));
        SetForScope knownTypeCell(m_knownTypeCell, cell.value());
        SetForScope knownCellType(m_knownCellType, &m_program.at(m_program.at(supertype).type));
        return dispatchOnShape(cell, field.subtypesWithField, joined, has, lacks);
    }
    return ifSubtype(cell, object, supertype, joined, has, lacks);
}

template<typename InlinePath, typename GenericPath>
Operand Lowering::ifSubtype(CellPointer cell, ValueID object, LayoutID supertype, JoinRep joined, const InlinePath& inlinePath, const GenericPath& genericPath)
{
    if (!m_program.at(supertype).acceptsUntypedObjects || isAllocatedAsSubtypeOf(object, supertype))
        return inlinePath();
    if (m_program.at(supertype).test.neverPasses())
        return genericPath();
    KnownSubtype known = std::exchange(m_knownSubtype, { });
    bool isKnown = known.cell == cell.value() && known.supertype == supertype && known.block == m_out.m_block && !mayHaveCalledSince(known.block, known.index, m_proc.size());
    Word32 condition = isKnown ? Word32(known.isSubtype) : passes(shapeWordOf(cell), m_program.at(supertype).test);
    std::optional<ValueFromBlock> fastPathIsSubtype;
    std::optional<ValueFromBlock> slowPathIsSubtype;
    Operand result = eitherOperand(condition, joined, [&] {
        LBasicBlock first = m_out.m_block;
        unsigned index = static_cast<unsigned>(first->size());
        unsigned numberOfBlocks = m_proc.size();
        Operand operand = inlinePath();
        if (!mayHaveCalledSince(first, index, numberOfBlocks))
            fastPathIsSubtype = m_out.anchor(m_out.booleanTrue);
        return operand;
    }, [&] {
        Operand operand = genericPath();
        if (fastPathIsSubtype)
            slowPathIsSubtype = m_out.anchor(passes(shapeWordOf(cell), m_program.at(supertype).test).value());
        return operand;
    });
    m_knownSubtype = { };
    if (slowPathIsSubtype) {
        LValue isSubtype = m_out.phi(B3::Int32, *fastPathIsSubtype, *slowPathIsSubtype);
        m_knownSubtype = KnownSubtype { cell.value(), supertype, isSubtype, m_out.m_block, static_cast<unsigned>(m_out.m_block->size()) };
    }
    return result;
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

template<typename InlinePath, typename GenericPath>
Operand Lowering::ifPredictionHolds(const Vector<Expectation, 4>& expectations, Rep rep, const InlinePath& inlinePath, const GenericPath& genericPath, LBasicBlock sharedSlowPath)
{
    LBasicBlock otherwise = sharedSlowPath ? sharedSlowPath : newColdBlock();
    Join join(m_out, rep, b3TypeOf(rep));
    size_t numberOfAssumptions = m_assumptions.size();
    for (auto& expectation : expectations) {
        if (!expectation.hasOriginalStructure && typeOf(expectation.value).isSubsetOf(*expectation.type, m_program, !expectation.rejectsUntypedObjects))
            continue;
        SetForScope rejectsUntypedObjects(m_rejectsUntypedObjects, m_rejectsUntypedObjects || expectation.rejectsUntypedObjects);
        m_assumptions.append(Assumption { expectation.value, speculateOrElse(expectation, otherwise), expectation.type });
    }
    SetForScope isOnConditionalPath(m_isOnConditionalPath, true);
    join.add(inlinePath(otherwise));
    m_assumptions.shrink(numberOfAssumptions);
    if (!sharedSlowPath) {
        m_out.appendTo(otherwise);
        join.add(genericPath());
    }
    return join.finish();
}

template<typename OnInt32, typename OnNumbers, typename GenericPath>
Operand Lowering::ifPredictedNumbers(std::initializer_list<ValueID> operands, Rep rep, bool hasCodeForInt32, const OnInt32& onInt32, const OnNumbers& onNumbers, const GenericPath& genericPath, bool acceptsAnyInteger, std::optional<std::pair<LBasicBlock, LBasicBlock>> successors)
{
    auto& int32Type = m_program.at(m_program.types.int32);
    auto& numberType = m_program.at(m_program.types.number);
    bool mayAllBeInt32 = hasCodeForInt32;
    bool mayBeOtherNumbers = false;
    bool hasUnboundedInteger = false;
    for (ValueID id : operands) {
        auto prediction = predictionOf(id);
        const Type& expected = prediction ? *prediction : typeOf(id);
        bool isInt32 = expected.isInteger() && (expected.integers.isSubsetOf(Range::int32()) || (acceptsAnyInteger && !prediction));
        mayAllBeInt32 &= isInt32 || (prediction && expected.primitives & Primitive::Int);
        mayBeOtherNumbers |= !isInt32;
        hasUnboundedInteger |= isInt32 && !expected.integers.isSubsetOf(Range::int32());
    }
    if (mayAllBeInt32 && (!m_isInLoop || hasUnboundedInteger))
        mayBeOtherNumbers = false;
    LBasicBlock otherwise = newColdBlock();
    std::optional<Join> join;
    if (!successors)
        join.emplace(m_out, rep, b3TypeOf(rep));
    auto yield = [&](Operand operand) {
        if (successors)
            m_out.branch(operand.value, unsure(successors->first), unsure(successors->second));
        else
            join->add(operand);
    };
    size_t numberOfAssumptions = m_assumptions.size();
    auto expect = [&](const Type& type, LBasicBlock onMismatch) {
        for (ValueID id : operands) {
            if (predictionOf(id))
                m_assumptions.append(Assumption { id, speculateOrElse(Expectation { id, &type }, onMismatch), &type });
        }
    };
    SetForScope isOnConditionalPath(m_isOnConditionalPath, true);
    unsigned predecessorCount = 0;
    if (mayAllBeInt32) {
        LBasicBlock first = m_out.m_block;
        LBasicBlock next = mayBeOtherNumbers ? m_out.newBlock() : otherwise;
        expect(int32Type, next);
        yield(onInt32(next));
        m_assumptions.shrink(numberOfAssumptions);
        if (mayBeOtherNumbers) {
            auto count = [&](LBasicBlock block) {
                for (LBasicBlock successor : block->successorBlocks())
                    predecessorCount += successor == next;
            };
            count(first);
            for (unsigned i = next->index() + 1; i < m_proc.size(); ++i)
                count(m_proc[i]);
            m_out.appendTo(next);
        }
    }
    if (!mayAllBeInt32 || mayBeOtherNumbers) {
        {
            SetForScope retestsOperands(m_retestsOperands, predecessorCount > 1);
            expect(numberType, otherwise);
        }
        yield(onNumbers());
        m_assumptions.shrink(numberOfAssumptions);
    }
    m_out.appendTo(otherwise);
    SetForScope hasFailedInt32Test(m_hasFailedInt32Test, mayAllBeInt32);
    yield(genericPath());
    return successors ? Operand() : join->finish();
}

template<typename GenericPath>
Operand Lowering::lowerPredictedBuiltin(ValueID result, const PredictedBuiltin& expected, std::optional<Boxed> callee, const GenericPath& genericPath)
{
    ValueID receiver = expected.receiver.isNever() ? ValueID() : expected.call.child(0);
    InlineBuiltin which = m_builtins.at(expected.call.builtin()).implementation.inlined;
    LBasicBlock sharedSlowPath = m_sharedGenericPath;
    SetForScope callerSharedPath(m_sharedGenericPath, nullptr);
    SetForScope sharedPathBuiltin(m_sharedGenericPathBuiltin, expected.call.builtin());
    Vector<Expectation, 4> expectations;
    bool readsMoreFromReceiver = m_builtins.at(expected.call.builtin()).thisUses & OperandUse::Gets && familyOf(which) != BuiltinFamily::Array;
    if (receiver)
        expectations.append(Expectation { receiver, &expected.receiver, (!callee || readsMoreFromReceiver) && which != InlineBuiltin::ArrayLength });
    for (unsigned i = 0; i < expected.arguments.size(); ++i)
        expectations.append(Expectation { expected.call.arguments[i], &expected.arguments[i] });
    bool needsAnyCheck = (receiver && callee) || expectations.containsIf([&](auto& expectation) {
        return expectation.hasOriginalStructure || !typeOf(expectation.value).isSubsetOf(*expectation.type, m_program);
    });
    bool takesObjectArgument = expected.arguments.containsIf([](auto& type) {
        return type.hasObjects();
    });
    if (needsAnyCheck && !takesObjectArgument && !expected.isFunctionCalledAsValue && prefersGenericPathInColdCode() && !m_function.at(result).isInTailPosition)
        return genericPath();
    auto& int32Type = m_program.at(m_program.types.int32);
    BuiltinID integerVariant;
    if (auto& builtin = m_builtins.at(expected.call.builtin()); !receiver && builtin.kind == BuiltinKind::StaticFunction && !expected.arguments.isEmpty()) {
        Vector<Type, 4> integers;
        integers.fill(int32Type, expected.arguments.size());
        integerVariant = applicableEntry(m_builtins.findStatic(BuiltinKind::StaticFunction, builtin.holderName, builtin.nameName), integers.span());
    }
    bool mayAllBeInt32 = integerVariant && m_builtins.at(integerVariant).parameters.begin()->conversion == ArgumentConversion::RequiresInteger;
    bool isAnyBoxed = false;
    for (unsigned i = 0; mayAllBeInt32 && i < expected.arguments.size(); ++i) {
        auto& known = typeOf(expected.call.arguments[i]);
        bool isBoxed = get(expected.call.arguments[i]).rep == Rep::JSValue && expected.arguments[i].isNumber() && !expected.arguments[i].isInteger();
        mayAllBeInt32 = isBoxed || (known.isInteger() && known.integers.isSubsetOf(Range::int32()));
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
                m_assumptions.append(Assumption { argument, speculateOrElse(Expectation { argument, &int32Type }, numbers), &int32Type });
        }
        SetForScope isOnConditionalPath(m_isOnConditionalPath, true);
        {
            SetForScope offer(m_sharedGenericPath, sharedSlowPath);
            join.add(*lowerInline(which, result, call));
        }
        m_assumptions.shrink(numberOfAssumptions);
        m_out.appendTo(numbers);
        join.add(ifPredictionHolds(expectations, rep, [&](LBasicBlock otherwise) {
            SetForScope offer(m_sharedGenericPath, otherwise);
            return *lowerInline(which, result, call);
        }, genericPath, sharedSlowPath));
        return join.finish();
    }
    return ifPredictionHolds(expectations, repOf(typeOf(result)), [&](LBasicBlock otherwise) {
        if (receiver && callee)
            orElse(equal(*callee, box(builtinCell(m_builtins.cellOf(expected.call.builtin())))), otherwise);
        Value call = expected.call;
        call.type = m_function.at(result).type;
        SetForScope predictedMethodAlreadyRead(m_predictedMethodAlreadyRead, receiver && callee ? expected.call.builtin() : m_predictedMethodAlreadyRead);
        SetForScope offer(m_sharedGenericPath, otherwise);
        return *lowerInline(which, result, call);
    }, genericPath, sharedSlowPath);
}

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

template<typename T, typename Functor>
T Lowering::lazilyInitialized(Stub stub, ptrdiff_t table, uint64_t indexAndMore, const Functor& fill)
{
    uint32_t index = static_cast<uint32_t>(indexAndMore);
    if (!m_isInLoop) {
        LValue entry = indexAndMore == index && thunkFor(stub, index)
            ? callAllocationStub(stub, { }, { Writes::RuntimeState, false }, false, false, false, nullptr, index, stub, index)
            : callAllocationStub(stub, { { m_out.constInt64(indexAndMore), lazyConstantKeyGPR } }, { Writes::RuntimeState, false }, false);
        entry->as<B3::PatchpointValue>()->pureFunctionKey = (static_cast<uint64_t>(stub) + 2) << 32 | index;
        return T(entry);
    }
    LBasicBlock slowPath = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    LValue value = table == Instance::offsetOfTypedClosures() ? closureIfCreated(index) : m_out.load64(m_out.address(m_heaps.AOTTyped_runtimeState, fixedPointer(table), static_cast<ptrdiff_t>(index) * sizeof(void*)));
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
constexpr OperationEffects effectsForOperation()
{
    return { WritesOf<operation>::value, OperationSignature<std::remove_pointer_t<decltype(operation)>>::mayThrow };
}

#define AOT_TYPED_OPERATION(name, ...) callOperation<operationAOTTyped ## name>(Entry::operationAOTTyped ## name, effectsForOperation<operationAOTTyped ## name>() __VA_OPT__(,) __VA_ARGS__)
#define AOT_TYPED_OPERATION_WITH_EFFECTS(name, effects, ...) callOperation<operationAOTTyped ## name>(Entry::operationAOTTyped ## name, effects __VA_OPT__(,) __VA_ARGS__)

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
