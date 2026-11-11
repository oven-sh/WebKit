/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTThrowEmitter.h"

#if ENABLE(AOT)

#include "AOTConversionEmitter.h"
#include "AOTEffectsDeclared.h"
#include "AOTExceptionEmitter.h"
#include "AOTFaultTable.h"
#include "AOTRepresentation.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTSlotRepresentation.h"
#include "AOTStubCalls.h"
#include "AOTStubCatalog.h"
#include "AOTStubRegisters.h"
#include "AOTStubsClosures.h"
#include "AOTStubsOperations.h"
#include "AOTStubsValues.h"
#include "AOTTypeArena.h"
#include "AOTVector.h"
#include "AllowMacroScratchRegisterUsage.h"
#include "JSCJSValueInlines.h"
#include "JSCell.h"
#include <algorithm>
#include <array>
#include <bit>
#include <type_traits>
#include <wtf/Hasher.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(numberOfEnumerators<ErrorKind> <= ThrowSiteWord::numberOfRegistersPerBank);
static_assert(JSValue::ValueNull < JSValue::ValueUndefined && static_cast<uintptr_t>(JSValue::ValueUndefined + JSCell::typeInfoTypeOffset()) < FaultSite::reach);
static_assert(nullCell32 < undefinedCell32 && (static_cast<uintptr_t>(undefinedCell32) << cell32Shift) + JSCell::typeInfoTypeOffset() < FaultSite::reach);
static_assert(nullCell32 == JSValue::ValueNull && undefinedCell32 == JSValue::ValueUndefined);

static constexpr std::array<ThrowKind, numberOfEnumerators<NullishAccess>> throwKindOfNullishAccess { ThrowKind::NullishRead, ThrowKind::NullishWrite, ThrowKind::NullishIterate, ThrowKind::NullishSpread, ThrowKind::NullishUpdate };
static_assert(allMembersOf<NullishAccess> == std::array { NullishAccess::Read, NullishAccess::Write, NullishAccess::Iterate, NullishAccess::Spread, NullishAccess::Update });

namespace {

struct ThrowSiteCode {
    ThrowSite site;
    Representation representation;
    unsigned registerNumberWithoutOperand;
    uint32_t callSite;
    OptionalIndex siteOfNewError { };
};

static_assert(std::is_trivially_destructible_v<ThrowSiteCode>);

} // anonymous namespace

Vector<uint32_t, 3> throwSiteWords(const ThrowSite& site, Representation representation, unsigned registerNumber)
{
    RELEASE_ASSERT(representation != Representation::Boolean);
    RELEASE_ASSERT(registerNumber < ThrowSiteWord::numberOfRegistersPerBank);
    RELEASE_ASSERT(!site.aIsMention || site.a != ThrowSiteWord::none);
    if (site.sourceOfFalseProof) {
        ThrowSite packed = site;
        packed.sourceOfFalseProof = std::nullopt;
        packed.calleeIsReadFromReceiver = true;
        packed.expectedProofType = 0;
        packed.a = FalseProofWord::encode(static_cast<uint32_t>(*site.sourceOfFalseProof), site.expectedProofType);
        return throwSiteWords(packed, representation, registerNumber);
    }
    bool hasWideA = site.aIsMention || (site.a > ThrowSiteWord::maxNarrowA && site.a != ThrowSiteWord::none);
    ThrowSiteWord word { };
    word.kind = static_cast<uint32_t>(site.kind);
    word.representation = static_cast<uint32_t>(representation);
    word.registerNumber = registerNumber;
    word.hasWideA = hasWideA;
    word.hasB = !!site.b;
    word.calleeIsReadFromReceiver = site.calleeIsReadFromReceiver;
    word.a = hasWideA ? 0 : std::min(site.a, ThrowSiteWord::narrowNone);
    Vector<uint32_t, 3> words { std::bit_cast<uint32_t>(word) };
    if (hasWideA)
        words.append(site.a);
    if (site.b)
        words.append(*site.b);
    return words;
}

RegisterSet registersClobberedByThrowSites(const StubCatalog& catalog)
{
    RegisterSet clobbered;
    for (ThrowSiteSource source : { ThrowSiteSource::WordsAfterCall, ThrowSiteSource::WordInRegister })
        clobbered.merge(catalog.summaryOf(throwAtSite(source, CallerFrame::HasFrame)).clobbered);
    return clobbered;
}

static void emitThrowSiteCode(CCallHelpers& jit, CodeRecords& records, const ThrowSiteCode& code, const Machine::PatchParameters& params)
{
    unsigned number = code.registerNumberWithoutOperand;
    bool valueIsInThrowWordRegister = false;
    if (code.representation == Representation::Double || code.representation == Representation::OptionalDouble)
        number = static_cast<unsigned>(params[0].fpr());
    else if (code.representation != Representation::Void) {
        number = static_cast<unsigned>(params[0].gpr());
        valueIsInThrowWordRegister = params[0].gpr() == throwWordGPR;
    }
    Vector<uint32_t, 3> words = throwSiteWords(code.site, code.representation, number);
    uint8_t wordsThatAreMentions = mentionsAmongThrowSiteWords(code.site, words.size());

    AllowMacroScratchRegisterUsage allowScratch(jit);
    ThrowSiteSource source = !code.site.b && !valueIsInThrowWordRegister ? ThrowSiteSource::WordInRegister : ThrowSiteSource::WordsAfterCall;
    StubKey key = throwAtSite(source, CallerFrame::HasFrame);
    if (!params.frameIsSetUp())
        jit.emitFunctionPrologue();
    if (source == ThrowSiteSource::WordInRegister) {
        RELEASE_ASSERT(words.size() <= 2);
        ArgumentMoves moves;
        appendArgumentMove(moves, throwWordGPR, wordsThatAreMentions ? ArgumentSource::ThrowWordWithMention : ArgumentSource::Constant, words.size() == 2 ? ThrowSiteWord::inRegister(words[0], words[1]) : words[0]);
        records.stubCalls.call(jit, key, std::nullopt, OptionalIndex { code.callSite }, moves, code.siteOfNewError);
        return;
    }
    records.stubCalls.callForTwoReads(jit, key, code.siteOfNewError, OptionalIndex { code.callSite });
    for (size_t index = 0; index < words.size(); ++index) {
#if CPU(X86_64)
        jit.m_assembler.buffer().putByte(static_cast<int8_t>(ThrowSiteWord::opcodeInFrontOfWordBehindCall));
#endif
        if (wordsThatAreMentions & (1 << index))
            records.relocations.emitNumber32(CodeSymbol { SymbolSpace::Number, words[index] });
        else
            jit.m_assembler.buffer().putInt(words[index]);
    }
}

static Machine::PatchGenerator* generatorOfThrowSite(Machine::Procedure& proc, CodeRecords& records, const ThrowSiteCode& code)
{
    return proc.createGenerator([records = &records, code](CCallHelpers& jit, const Machine::PatchParameters& params) {
        emitThrowSiteCode(jit, *records, code, params);
    });
}

static Representation representationInThrowWords(const std::optional<Operand>& operand)
{
    if (!operand)
        return Representation::Void;
    RELEASE_ASSERT(canBeNamedByThrowWordsOf(operand->representation));
    return operand->representation == Representation::Cell ? Representation::JSValue : operand->representation;
}

static std::optional<Operand> withoutUndefinedConstant(const std::optional<Operand>& operand)
{
    if (!operand || operand->representation == Representation::Void)
        return std::nullopt;
    if (operand->representation == Representation::JSValue && operand->value->hasInt64() && operand->value->asInt64() == JSValue::encode(jsUndefined()))
        return std::nullopt;
    return operand;
}

static ThrowSite checkedThrowSite(const ThrowSite& site)
{
    std::optional<uint32_t> b = site.b == ThrowSiteWord::none ? std::nullopt : site.b;
    RELEASE_ASSERT(site.a == ThrowSiteWord::none ? !site.aIsMention : !(site.a >> 31));
    RELEASE_ASSERT(!(b.value_or(0) >> 31));
    return { .kind = site.kind, .a = site.a, .b = b, .aIsMention = site.aIsMention, .bIsMention = b && site.bIsMention, .sourceOfFalseProof = site.sourceOfFalseProof, .expectedProofType = site.expectedProofType };
}

static ThrowSite throwSiteWithName(MentionList& mentions, ThrowKind kind, StringID name)
{
    if (!name)
        return { kind, ThrowSiteWord::none, std::nullopt };
    return { .kind = kind, .a = mentions.add(NumberSpace::ShownString, name), .b = std::nullopt, .aIsMention = true };
}

static bool isCellOrNullish(const Type& type)
{
    return !(type.primitives & ~(cellPrimitives | Primitive::UndefinedOrNull));
}

ThrowEmitter::ThrowEmitter(CodeGenerator& generator)
    : OperationEmitter(generator)
    , m_clobberedRegisters(generator.emitter<RuntimeCallEmitter>().registersThatThrowSitesClobber())
{
}

Operand ThrowEmitter::inFormNamedByThrowWords(const OperationContext& context, Operand operand)
{
    switch (operand.representation) {
    case Representation::Boolean:
        return boxBoolean(operand.as<Condition>());
    case Representation::Int32:
        return signExtended(operand.as<Word32>());
    case Representation::Cell32:
        return emitter<ConversionEmitter>().decode(operand.as<NarrowReference>(), types().at(widestTypeOf(Representation::Cell32)), Representation::JSValue);
    case Representation::JSValue:
    case Representation::Int64:
    case Representation::Double:
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
    case Representation::Cell:
        return operand;
    case Representation::Void:
    case Representation::RawWord:
    case Representation::Unchosen:
        break;
    }
    fail(context, InternalErrorKind::UnsupportedConversion);
    return undefined();
}

void ThrowEmitter::throwAt(const OperationContext& context, const ThrowSite& site, std::optional<Operand> operand)
{
    throwAt(context, site, operand, std::nullopt);
}

void ThrowEmitter::throwAt(const OperationContext& context, const ThrowSite& site, std::optional<Operand> operand, std::optional<NewError> newError)
{
    RELEASE_ASSERT(!generator().isInitializingCell());
    operand = withoutUndefinedConstant(operand);
    if (operand && !canBeNamedByThrowWordsOf(operand->representation))
        operand = inFormNamedByThrowWords(context, *operand);

    RuntimeCallEmitter& calls = emitter<RuntimeCallEmitter>();
    static_assert(!declaredBoundOf(RuntimeOperation::ThrowAtSite).mayCollect);
    Effects effects = Effects::none();
    effects.mayThrow = true;
    CallPatchpointRequest request {
        .resultType = Machine::Void,
        .earlyClobbered = m_clobberedRegisters,
        .lateClobbered = m_clobberedRegisters,
        .effects = effects,
        .argumentBufferAccess = ArgumentBufferAccess::None,
        .runsWithoutFrame = false,
        .neverReturns = true,
    };
    Machine::PatchOperation* patchpoint = calls.createCallPatchpoint(context, request);
    LBasicBlock siteBlock = m_out.m_block;
    if (operand)
        patchpoint->appendSomeRegister(operand->value);
    ThrowSiteCode code { checkedThrowSite(site), representationInThrowWords(operand), newError ? static_cast<unsigned>(newError->kind) : 0, 0 };
    if (newError && newError->throwPosition) {
        Origin throwOrigin = context.origin();
        throwOrigin.position = *newError->throwPosition;
        code.siteOfNewError = { calls.addSite(context) };
        code.callSite = calls.addSite(context, { .origin = throwOrigin });
    } else
        code.callSite = calls.addSite(context);
    patchpoint->setGenerator(generatorOfThrowSite(m_proc, m_records, code));
    calls.finishCallPatchpoint(context, patchpoint, code.callSite, request);
    if (m_out.m_block == siteBlock)
        m_out.appendTo(newBlock());
}

void ThrowEmitter::requireOrThrow(const OperationContext& context, Condition mustHold, const ThrowSite& site, std::optional<Operand> operand)
{
    RELEASE_ASSERT(!generator().isInitializingCell());
    if (mustHold.value()->isInt32(1))
        return;
    if (mustHold.value()->isInt32(0)) {
        throwAt(context, site, operand);
        return;
    }
    operand = withoutUndefinedConstant(operand);
    bool operandNeedsConversion = operand && !canBeNamedByThrowWordsOf(operand->representation);
    if (operandNeedsConversion || (!site.sourceOfFalseProof && ExceptionEmitter::unwindsWithinFunction(context))) {
        LBasicBlock fails = newBlock();
        LBasicBlock continuation = newBlock();
        ScopeFork fork = m_out.forkScope();
        m_out.branch(mustHold.value(), normalEdge(continuation), rareEdge(fails));
        m_out.appendToArm(fails, fork);
        throwAt(context, site, operand);
        m_out.unreachable();
        m_out.appendToArm(continuation, fork);
        return;
    }

    Machine::PatchOperation* check = m_out.checkIsZero(logicalNot(mustHold).value());
    if (operand)
        check->appendSomeRegister(operand->value);
    check->clobberEarly(m_clobberedRegisters);
    check->setRunsWithoutFrame(true);
    check->setSharesLatePath(true);
    check->setGenerator(generatorOfCheckedThrowSite(context, checkedThrowSite(site), representationInThrowWords(operand)));
}

Machine::PatchGenerator* ThrowEmitter::generatorOfCheckedThrowSite(const OperationContext& context, const ThrowSite& site, Representation representation)
{
    RuntimeCallEmitter& calls = emitter<RuntimeCallEmitter>();
    RecordedSite position = calls.recordedSiteOf(context);
    auto& generators = m_checkedThrowSites.add(computeHash(site.kind, site.a, site.b, site.sourceOfFalseProof, site.expectedProofType, representation, position.origin.position, position.origin.inlineFrame), Vector<CheckedThrowSite, 1>()).iterator->value;
    for (const CheckedThrowSite& known : generators) {
        if (known.site == site && known.representation == representation && known.position == position)
            return known.generator;
    }

    Machine::PatchGenerator* generator = generatorOfThrowSite(m_proc, m_records, { site, representation, 0, calls.addRecordedSite(context, position) });
    generators.append({ site, representation, position, generator });
    return generator;
}

void ThrowEmitter::throwError(const OperationContext& context, ErrorKind kind, StringID message, std::optional<uint32_t> throwPosition)
{
    throwAt(context, throwSiteWithName(m_mentions, ThrowKind::ErrorWithMessage, message), std::nullopt, NewError { kind, throwPosition });
}

static ThrowSite siteOfFalseProof(ProofSource source, uint32_t expectedType)
{
    static_assert(numberOfEnumerators<ProofSource> <= 1u << FalseProofWord::sourceBits);
    static_assert(FalseProofWord::encode(numberOfEnumerators<ProofSource>, FalseProofWord::everyBit) <= ThrowSiteWord::maxNarrowA);
    return { .kind = ThrowKind::ErrorWithMessage, .a = ThrowSiteWord::none, .b = std::nullopt, .sourceOfFalseProof = source, .expectedProofType = expectedType };
}

void ThrowEmitter::requireOrCrash(const OperationContext& context, Condition holds, std::optional<Boxed> found, ProofSource source, uint32_t expectedType)
{
    LBasicBlock stops = newBlock();
    LBasicBlock continuation = newBlock();
    ScopeFork fork = m_out.forkScope();
    m_out.branch(holds.value(), normalEdge(continuation), rareEdge(stops));
    m_out.appendToArm(stops, fork);
    crashOnFalseProof(context, found, source, expectedType);
    m_out.appendToArm(continuation, fork);
}

void ThrowEmitter::checkOrCrash(const OperationContext& context, Condition holds, ProofSource source, uint32_t expectedType)
{
    if (generator().isInitializingCell())
        requireOrCrash(context, holds, std::nullopt, source, expectedType);
    else
        requireOrThrow(context, holds, siteOfFalseProof(source, expectedType), std::nullopt);
}

void ThrowEmitter::crashOnFalseProof(const OperationContext& context, std::optional<Boxed> found, ProofSource source, uint32_t expectedType)
{
    if (!generator().isInitializingCell()) {
        throwAt(context, siteOfFalseProof(source, expectedType), found ? std::optional<Operand>(*found) : std::nullopt);
        m_out.unreachable();
        return;
    }
    Origin origin = context.origin();
    FunctionID writtenIn = generator().code().inlineFrames()[origin.inlineFrame].callee;
    m_out.suspendDeclaredAccesses();
    callVoidOperation(context, RuntimeOperation::StopAtFalseProof, { found ? *found : Boxed(m_out.constInt64(JSValue::encode(JSValue()))), word32(static_cast<int32_t>(writtenIn.index())), word32(static_cast<int32_t>(origin.position)), word32(static_cast<int32_t>(FalseProofWord::encode(static_cast<uint32_t>(source), expectedType))) });
    m_out.resumeDeclaredAccesses();
    m_out.unreachable();
}

void ThrowEmitter::stopUnlessStubPasses(const OperationContext& context, Boxed found, StubKey stub, uint32_t kinds, ProofSource source, std::optional<uint32_t> mentionAboveKinds, std::optional<uint32_t> numberInRegister)
{
    RELEASE_ASSERT(!generator().isInitializingCell());
    Machine::PatchOperation* check = m_out.patchpoint(Machine::Void);
    check->effects = Machine::Effects::none();
    check->effects.exitsSideways = true;
    check->effects.controlDependent = true;
    check->effects.reads = Machine::HeapSet::top();
    check->clobber(inputs().stubCatalog.clobberedByRegisterVariantsOf(stub));
    check->appendSomeRegister(found.value());
    if (numberInRegister)
        check->append(Machine::ConstrainedValue(numberOf(*numberInRegister).value(), Machine::ValueRep::reg(signatureOf(stub).operands[2].gpr())));
    uint32_t callSite = emitter<RuntimeCallEmitter>().addSite(context);
    check->setGenerator([records = &m_records, stub, words = KindsOfProof::withSource(kinds, source), mentionAboveKinds, callSite](CCallHelpers& jit, const Machine::PatchParameters& params) {
        GPRReg value = params[0].gpr();
        StubKey key = stub;
        if (value != signatureOf(key).operands[0].gpr())
            key = key.withOperandRegister(value);
        RELEASE_ASSERT(params.size() < 2 || params[1].gpr() == signatureOf(key).operands[2].gpr());
        ArgumentMoves moves;
        if (mentionAboveKinds)
            appendArgumentMove(moves, throwWordGPR, ArgumentSource::ConstantBelowMention, words | static_cast<uint64_t>(*mentionAboveKinds) << KindsOfProof::shiftOfNumber);
        else
            appendArgumentMove(moves, throwWordGPR, ArgumentSource::Constant, words);
        records->stubCalls.call(jit, key, std::nullopt, OptionalIndex { callSite }, moves);
    });
}

void ThrowEmitter::requireNotNullish(const OperationContext& context, Operand operand, const Type& type, NullishAccess access, StringID name)
{
    if (!type.mayBeUndefinedOrNull())
        return;
    RELEASE_ASSERT(static_cast<size_t>(access) < throwKindOfNullishAccess.size());
    ThrowSite site = throwSiteWithName(m_mentions, throwKindOfNullishAccess[static_cast<size_t>(access)], name);
    switch (operand.representation) {
    case Representation::JSValue:
        if (isCellOrNullish(type) && canTrapAt(context))
            trapThrough(context, operand.value, Representation::JSValue, site);
        else
            requireOrThrow(context, logicalNot(isUndefinedOrNull(operand.as<Boxed>(), type.primitives)), site, operand);
        return;
    case Representation::Cell32: {
        NarrowReference word = operand.as<NarrowReference>();
        if (isCellOrNullish(type) && canTrapAt(context))
            trapThrough(context, decode(word).value(), Representation::Cell, site);
        else if (isCellOrNullish(type))
            requireOrThrow(context, isCellReference(word), site, operand);
        else {
            Condition isNull = equal(word, NarrowReference(m_out.constInt32(nullCell32)));
            Condition isUndefined = equal(word, NarrowReference(m_out.constInt32(undefinedCell32)));
            requireOrThrow(context, logicalNot(logicalOr(isNull, isUndefined)), site, operand);
        }
        return;
    }
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
        requireOrThrow(context, logicalNot(emitter<ConversionEmitter>().isMarker(operand)), site, operand);
        return;
    case Representation::Cell:
    case Representation::Int32:
    case Representation::Int64:
    case Representation::Double:
    case Representation::Boolean:
        return;
    case Representation::Void:
        if (type.isUndefinedOrNull()) {
            throwAt(context, site, Operand(boxedConstant(type.primitives & Primitive::Null ? jsNull() : jsUndefined())));
            return;
        }
        break;
    case Representation::RawWord:
    case Representation::Unchosen:
        break;
    }
    fail(context, InternalErrorKind::OpcodeWithoutCode);
}

bool ThrowEmitter::canTrapAt(const OperationContext& context)
{
    if (!context.valueID() || ExceptionEmitter::unwindsWithinFunction(context))
        return false;
    const Vector<TrapSite>& trapSites = m_records.traps.trapSites;
    return trapSites.isEmpty() || trapSites.last().value != context.valueID();
}

void ThrowEmitter::trapThrough(const OperationContext& context, LValue pointer, Representation representation, const ThrowSite& site)
{
    RELEASE_ASSERT(!generator().isInitializingCell());
    RELEASE_ASSERT(canTrapAt(context) && valueOfOrigin(m_out.origin()) == context.valueID());
    RELEASE_ASSERT(!site.b);
    TypedPointer typeOfCell = m_out.address(pointer, m_heaps.JSCell_typeInfoType);
    if (std::optional<int64_t> known = m_out.valueKnownToBeReadThrough(typeOfCell)) {
        unsigned shift = pointer->hasInt64() ? 0 : cell32Shift;
        bool isNull = *known == static_cast<int64_t>(JSValue::ValueNull) << shift;
        bool isUndefined = *known == static_cast<int64_t>(JSValue::ValueUndefined) << shift;
        if (isNull || isUndefined)
            throwAt(context, site, Operand(boxedConstant(isNull ? jsNull() : jsUndefined())));
        return;
    }
    m_out.load8ZeroExt32Trapping(typeOfCell);
    m_records.traps.trapSites.append(TrapSite { context.valueID(), checkedThrowSite(site), representation, emitter<RuntimeCallEmitter>().addSite(context) });
}

void ThrowEmitter::requireInitialized(const OperationContext& context, Operand operand, const Type& type, StringID name)
{
    if (!(type.primitives & Primitive::Absent))
        return;
    ThrowSite site = throwSiteWithName(m_mentions, ThrowKind::UninitializedBinding, name);
    if (operand.representation == Representation::Void) {
        throwAt(context, site, std::nullopt);
        return;
    }
    bool isCellOrAbsent = !(type.primitives & ~(cellPrimitives | Primitive::Absent));
    if (isCellOrAbsent && operand.representation == Representation::JSValue && !ConversionEmitter::markerBits(Primitive::Absent, Representation::JSValue) && !operand.value->hasInt64() && canTrapAt(context)) {
        trapThrough(context, operand.value, Representation::JSValue, site);
        return;
    }
    requireOrThrow(context, logicalNot(emitter<ConversionEmitter>().isAbsent(operand)), site, std::nullopt);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
