/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTThrowEmitter.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCodeUnits.h"
#include "AOTConversionEmitter.h"
#include "AOTExceptionEmitter.h"
#include "AOTFaultTable.h"
#include "AOTPlacement.h"
#include "AOTRepresentation.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTSlotRepresentation.h"
#include "AOTStubCalls.h"
#include "AOTStubCatalog.h"
#include "AOTStubRegisters.h"
#include "AOTStubsOperations.h"
#include "AOTVector.h"
#include "AllowMacroScratchRegisterUsage.h"
#include "B3ArgumentRegValue.h"
#include "B3BasicBlockInlines.h"
#include "B3CheckValue.h"
#include "B3Const32Value.h"
#include "B3Const64Value.h"
#include "B3InsertionSetInlines.h"
#include "B3MemoryValue.h"
#include "B3PatchpointValue.h"
#include "B3Procedure.h"
#include "B3ReduceStrength.h"
#include "B3StackmapGenerationParams.h"
#include "B3ValueInlines.h"
#include "JSCJSValueInlines.h"
#include "JSCell.h"
#include <bit>
#include <wtf/Hasher.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(numberOfEnumerators<ErrorKind> <= ThrowSiteWord::numberOfRegistersPerBank);
static_assert(JSValue::ValueNull < JSValue::ValueUndefined && static_cast<uintptr_t>(JSValue::ValueUndefined + JSCell::typeInfoTypeOffset()) < FaultSite::reach);
static_assert(nullCell32 < undefinedCell32 && (static_cast<uintptr_t>(undefinedCell32) << cell32Shift) + JSCell::typeInfoTypeOffset() < FaultSite::reach);
static_assert(traitsOf(SlotRepresentation::LiteralString).undefinedBits == JSValue::ValueUndefined && traitsOf(SlotRepresentation::LiteralString).nullBits == JSValue::ValueNull);

namespace {

struct ThrowSiteCode {
    ThrowSite site;
    Representation representation;
    unsigned registerNumberWithoutOperand;
    uint32_t callSite;
};

class ThrownOperandEmitter final : public OperationEmitter {
public:
    explicit ThrownOperandEmitter(CodeGenerator& generator)
        : OperationEmitter(generator)
    {
    }

    Operand thrownOperand(const OperationContext&, Operand, const Type&);
};

} // anonymous namespace

Vector<uint32_t, 3> throwSiteWords(const ThrowSite& site, Representation representation, unsigned registerNumber)
{
    RELEASE_ASSERT(representation != Representation::LiteralString && representation != Representation::Boolean);
    RELEASE_ASSERT(registerNumber < ThrowSiteWord::numberOfRegistersPerBank);
    bool hasWideA = site.a > ThrowSiteWord::maxNarrowA;
    ThrowSiteWord word { };
    word.kind = static_cast<uint32_t>(site.kind);
    word.representation = static_cast<uint32_t>(representation);
    word.registerNumber = registerNumber;
    word.hasWideA = hasWideA;
    word.hasB = !!site.b;
    word.a = hasWideA ? 0 : site.a;
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

static void emitThrowSiteCall(CCallHelpers& jit, CodeRecords& records, bool frameIsSetUp, uint32_t site, const Vector<uint32_t, 3>& words, bool valueIsInThrowWordRegister)
{
    AllowMacroScratchRegisterUsage allowScratch(jit);
    ThrowSiteSource source = words.size() == 1 && !valueIsInThrowWordRegister ? ThrowSiteSource::WordInRegister : ThrowSiteSource::WordsAfterCall;
    StubKey key = throwAtSite(source, CallerFrame::HasFrame);
    // The path never returns, and nothing is saved yet: a bare frame record makes the function visible to the stack walk.
    if (!frameIsSetUp)
        jit.emitFunctionPrologue();
    if (source == ThrowSiteSource::WordInRegister) {
        ArgumentMoves moves;
        appendArgumentMove(moves, throwWordGPR, ArgumentSource::Constant, words[0]);
        records.stubCalls.call(jit, key, std::nullopt, OptionalIndex { site }, moves);
        return;
    }
    records.stubCalls.call(jit, key, OptionalIndex { site });
    CCallHelpers::Label begin = jit.labelIgnoringWatchpoints();
    for (uint32_t word : words)
        jit.m_assembler.buffer().putInt(word);
    records.units.pin(begin, jit.labelIgnoringWatchpoints());
}

static void emitThrowSiteCode(CCallHelpers& jit, CodeRecords& records, bool frameIsSetUp, const ThrowSiteCode& code, const B3::StackmapGenerationParams& params)
{
    unsigned number = code.registerNumberWithoutOperand;
    bool valueIsInThrowWordRegister = false;
    if (code.representation == Representation::Double || code.representation == Representation::OptionalDouble)
        number = static_cast<unsigned>(params[0].fpr());
    else if (code.representation != Representation::Void) {
        number = static_cast<unsigned>(params[0].gpr());
        valueIsInThrowWordRegister = params[0].gpr() == throwWordGPR;
    }
    emitThrowSiteCall(jit, records, frameIsSetUp, code.callSite, throwSiteWords(code.site, code.representation, number), valueIsInThrowWordRegister);
}

static bool canBeNamedByThrowWords(Representation representation)
{
    switch (representation) {
    case Representation::JSValue:
    case Representation::Int64:
    case Representation::Double:
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
    case Representation::Cell:
        return true;
    case Representation::Boolean:
    case Representation::Int32:
    case Representation::Cell32:
    case Representation::LiteralString:
    case Representation::Void:
    case Representation::RawWord:
    case Representation::Unchosen:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Representation representationInThrowWords(const std::optional<Operand>& operand)
{
    if (!operand)
        return Representation::Void;
    RELEASE_ASSERT(canBeNamedByThrowWords(operand->representation));
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
    RELEASE_ASSERT(site.a == ThrowSiteWord::none || !(site.a >> 31));
    RELEASE_ASSERT(!(b.value_or(0) >> 31));
    return { site.kind, site.a, b };
}

static uint32_t nameWord(StringID name)
{
    return name ? name.index() : ThrowSiteWord::none;
}

static bool isCellOrNullish(const Type& type)
{
    return !(type.primitives & ~(cellPrimitives | Primitive::UndefinedOrNull));
}

Operand ThrownOperandEmitter::thrownOperand(const OperationContext& context, Operand operand, const Type& type)
{
    switch (operand.representation) {
    case Representation::Boolean:
        return boxBoolean(operand.as<Condition>());
    case Representation::Int32:
        return signExtended(operand.as<Word32>());
    case Representation::Cell32:
        return emitter<ConversionEmitter>().decode(operand.as<NarrowReference>(), type, Representation::JSValue);
    case Representation::LiteralString:
        return emitter<ConversionEmitter>().boxLiteralString(context, operand.as<LiteralStringWord>(), type);
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

ThrowEmitter::ThrowEmitter(CodeGenerator& generator)
    : OperationEmitter(generator)
    , m_clobberedRegisters(registersClobberedByThrowSites(generator.inputs().program.stubCatalog))
{
}

void ThrowEmitter::emitThrowSite(const OperationContext& context, const ThrowSite& site, std::optional<Operand> operand, std::optional<ErrorKind> errorKind)
{
    RELEASE_ASSERT(!generator().isInitializingCell());
    operand = withoutUndefinedConstant(operand);
    if (operand && !canBeNamedByThrowWords(operand->representation))
        operand = ThrownOperandEmitter(generator()).thrownOperand(context, *operand, widestTypeOf(operand->representation));

    build().statistics().count(StatisticID::ThrowSites);
    RuntimeCallEmitter& calls = emitter<RuntimeCallEmitter>();
    Effects effects = Effects::none();
    effects.mayThrow = true;
    CallPatchpointRequest request {
        .resultType = B3::Void,
        .earlyClobbered = m_clobberedRegisters,
        .lateClobbered = m_clobberedRegisters,
        .effects = effects,
        .argumentBufferAccess = ArgumentBufferAccess::None,
        .runsWithoutFrame = false,
        .neverReturns = true,
    };
    B3::PatchpointValue* patchpoint = calls.createCallPatchpoint(context, request);
    LBasicBlock siteBlock = m_out.m_block;
    if (operand)
        patchpoint->append(B3::ConstrainedValue(operand->value, B3::ValueRep::SomeRegister));
    ThrowSiteCode code { checkedThrowSite(site), representationInThrowWords(operand), errorKind ? static_cast<unsigned>(*errorKind) : 0, calls.addSite(context) };
    patchpoint->setGenerator([records = &m_records, code](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
        emitThrowSiteCode(jit, *records, params.context().frameIsSetUp, code, params);
    });
    calls.finishCallPatchpoint(context, patchpoint, code.callSite, request);
    if (m_out.m_block == siteBlock)
        m_out.appendTo(newBlock());
}

void ThrowEmitter::throwAt(const OperationContext& context, const ThrowSite& site, std::optional<Operand> operand)
{
    emitThrowSite(context, site, operand);
}

void ThrowEmitter::requireOrThrow(const OperationContext& context, Condition mustHold, const ThrowSite& site, std::optional<Operand> operand)
{
    RELEASE_ASSERT(!generator().isInitializingCell());
    if (mustHold.value()->isInt32(1))
        return;
    operand = withoutUndefinedConstant(operand);
    bool operandNeedsConversion = operand && !canBeNamedByThrowWords(operand->representation);
    if (operandNeedsConversion || emitter<ExceptionEmitter>().unwindsWithinFunction(context)) {
        branchOrThrow(context, mustHold, site, operand);
        return;
    }
    checkOrThrow(context, mustHold, site, operand);
}

void ThrowEmitter::checkOrThrow(const OperationContext& context, Condition mustHold, const ThrowSite& site, std::optional<Operand> operand)
{
    B3::CheckValue* check = m_out.checkIsZero(logicalNot(mustHold).value());
    if (operand)
        check->append(B3::ConstrainedValue(operand->value, B3::ValueRep::SomeRegister));
    check->clobberEarly(m_clobberedRegisters);
    check->setRunsWithoutFrame(true);
    check->setSharesLatePath(true);
    check->setGenerator(generatorOfCheckedThrowSite(context, checkedThrowSite(site), representationInThrowWords(operand)));
}

// The tests of one type, and the copies that a pass made of one test, fail the same way at the same position.
RefPtr<B3::StackmapGenerator> ThrowEmitter::generatorOfCheckedThrowSite(const OperationContext& context, const ThrowSite& site, Representation representation)
{
    RuntimeCallEmitter& calls = emitter<RuntimeCallEmitter>();
    RecordedSite position = calls.recordedSiteOf(context);
    auto& generators = m_checkedThrowSites.add(computeHash(site.kind, site.a, site.b, representation, position.origin.position, position.origin.inlineFrame), Vector<CheckedThrowSite, 1>()).iterator->value;
    for (const CheckedThrowSite& known : generators) {
        if (known.site == site && known.representation == representation && known.position == position)
            return known.generator;
    }

    build().statistics().count(StatisticID::ThrowSites);
    ThrowSiteCode code { site, representation, 0, calls.addSite(context) };
    RefPtr<B3::StackmapGenerator> generator = createSharedTask<B3::StackmapGeneratorFunction>([records = &m_records, code](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
        emitThrowSiteCode(jit, *records, params.context().frameIsSetUp, code, params);
    });
    generators.append({ site, representation, position, generator });
    return generator;
}

void ThrowEmitter::branchOrThrow(const OperationContext& context, Condition mustHold, const ThrowSite& site, std::optional<Operand> operand)
{
    LBasicBlock fails = newBlock();
    LBasicBlock continuation = newBlock();
    m_out.branch(mustHold.value(), normalEdge(continuation), rareEdge(fails));
    m_out.appendTo(fails);
    emitThrowSite(context, site, operand);
    m_out.unreachable();
    m_out.appendTo(continuation);
}

void ThrowEmitter::throwError(const OperationContext& context, ErrorKind kind, StringID message)
{
    emitThrowSite(context, { ThrowKind::ErrorWithMessage, message.index(), std::nullopt }, std::nullopt, kind);
}

ThrowKind ThrowEmitter::throwKindOf(NullishAccess access)
{
    switch (access) {
    case NullishAccess::Read:
        return ThrowKind::NullishRead;
    case NullishAccess::Write:
        return ThrowKind::NullishWrite;
    case NullishAccess::Iterate:
        return ThrowKind::NullishIterate;
    case NullishAccess::Spread:
        return ThrowKind::NullishSpread;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void ThrowEmitter::requireNotNullish(const OperationContext& context, Operand operand, const Type& type, NullishAccess access, StringID name)
{
    if (!type.mayBeUndefinedOrNull())
        return;
    ThrowSite site { throwKindOf(access), nameWord(name), std::nullopt };
    switch (operand.representation) {
    case Representation::JSValue: {
        Boxed value = operand.as<Boxed>();
        if (isCellOrNullish(type) && canTrapAt(context)) {
            trapIfNullish(context, value, site);
            return;
        }
        requireOrThrow(context, logicalNot(isUndefinedOrNull(value, type.primitives)), site, operand);
        return;
    }
    case Representation::Cell32: {
        NarrowReference word = operand.as<NarrowReference>();
        if (isCellOrNullish(type) && canTrapAt(context)) {
            trapIfNullish(context, word, site);
            return;
        }
        if (isCellOrNullish(type)) {
            requireOrThrow(context, isCellReference(word), site, operand);
            return;
        }
        Condition isNull = equal(word, NarrowReference(m_out.constInt32(nullCell32)));
        Condition isUndefined = equal(word, NarrowReference(m_out.constInt32(undefinedCell32)));
        requireOrThrow(context, logicalNot(logicalOr(isNull, isUndefined)), site, operand);
        return;
    }
    case Representation::LiteralString: {
        // A word that fails is a marker, and a marker is its own boxed form.
        Boxed marker(operand.value);
        requireOrThrow(context, logicalNot(isUndefinedOrNull(marker, type.primitives)), site, Operand(marker));
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
    if (!context.valueID() || emitter<ExceptionEmitter>().unwindsWithinFunction(context))
        return false;
    const Vector<TrapSite>& trapSites = m_records.traps.trapSites;
    return trapSites.isEmpty() || trapSites.last().value != context.valueID();
}

void ThrowEmitter::trapIfNullish(const OperationContext& context, Boxed value, const ThrowSite& site)
{
    trapThrough(context, value.value(), Representation::JSValue, site);
}

// The first pages of the cage of cells are never mapped, and a marker is a small word: what it decodes to lies in them.
void ThrowEmitter::trapIfNullish(const OperationContext& context, NarrowReference word, const ThrowSite& site)
{
    trapThrough(context, decode(word).value(), Representation::Cell, site);
}

void ThrowEmitter::trapThrough(const OperationContext& context, LValue pointer, Representation representation, const ThrowSite& site)
{
    RELEASE_ASSERT(!generator().isInitializingCell());
    RELEASE_ASSERT(canTrapAt(context) && valueOfOrigin(m_out.origin()) == context.valueID());
    RELEASE_ASSERT(!site.b);
    m_out.load8ZeroExt32Trapping(m_out.address(pointer, m_heaps.JSCell_typeInfoType));
    m_records.traps.trapSites.append(TrapSite { context.valueID(), checkedThrowSite(site), representation, emitter<RuntimeCallEmitter>().addSite(context) });
}

static bool isCellCageBase(const B3::Value* value)
{
    auto* argument = value->as<B3::ArgumentRegValue>();
    return argument && argument->argumentReg() == Reg(cellCageBaseGPR);
}

// The boxed value, also where the access goes through what a narrow reference decodes to.
static std::optional<int64_t> findValueKnownToBeReadThrough(const B3::MemoryValue* access)
{
    const B3::Value* base = access->lastChild();
    int64_t distance = static_cast<int64_t>(access->offset()) - JSCell::typeInfoTypeOffset();
    if (base->hasIntPtr())
        return base->asIntPtr() + distance;
    if (base->opcode() == B3::Add && isCellCageBase(base->child(0)) && base->child(1)->hasIntPtr()) {
        distance += base->child(1)->asIntPtr();
        base = base->child(0);
    }
    if (!isCellCageBase(base) || distance < 0 || distance & ((1 << cell32Shift) - 1))
        return std::nullopt;
    return distance >> cell32Shift;
}

static bool throwWhereTrapIsCertain(B3::Procedure& proc, CodeRecords& records, const RegisterSet& clobberedRegisters)
{
    if (records.traps.trapSites.isEmpty())
        return false;

    bool changed = false;
    B3::InsertionSet insertionSet(proc);
    for (B3::BasicBlock* block : proc) {
        for (unsigned index = 0; index < block->size(); ++index) {
            auto* access = block->at(index)->as<B3::MemoryValue>();
            if (!access || !access->traps())
                continue;
            std::optional<int64_t> known = findValueKnownToBeReadThrough(access);
            if (!known || (*known != JSValue::ValueNull && *known != JSValue::ValueUndefined))
                continue;
            int64_t bits = *known;
            const Vector<TrapSite>& trapSites = records.traps.trapSites;
            size_t site = trapSites.findIf([&](const TrapSite& candidate) {
                return candidate.value == valueOfOrigin(access->origin());
            });
            RELEASE_ASSERT(site != notFound);

            B3::Origin origin = access->origin();
            // The thrower takes a site that names no operand for one of undefined.
            bool namesOperand = bits != JSValue::ValueUndefined;
            B3::Value* operand = namesOperand ? insertionSet.insert<B3::Const64Value>(index, origin, bits) : nullptr;
            B3::CheckValue* check = insertionSet.insert<B3::CheckValue>(index, B3::Check, origin, insertionSet.insert<B3::Const32Value>(index, origin, 1));
            if (operand)
                check->append(B3::ConstrainedValue(operand, B3::ValueRep::SomeRegister));
            check->clobberEarly(clobberedRegisters);
            check->setRunsWithoutFrame(true);
            ThrowSiteCode code { trapSites[site].throwSite, namesOperand ? Representation::JSValue : Representation::Void, 0, trapSites[site].callSite };
            check->setGenerator([records = &records, code](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
                emitThrowSiteCode(jit, *records, params.context().frameIsSetUp, code, params);
            });
            changed = true;
            break;
        }
        insertionSet.execute(block);
    }
    // A check that always fails ends its block.
    if (changed)
        B3::reduceStrength(proc, B3::ReduceStrengthPass::Final);
    return changed;
}

// The handler of a fault reads the tested value out of the base register of the access that faulted.
B3::ClientPhase throwWhereTrapsAreCertain(CodeRecords& records, const StubCatalog& catalog)
{
    return {
        B3::ClientPhase::Point::AfterOptimization,
        "throwWhereTrapsAreCertain"_s,
        createSharedTask<bool(B3::Procedure&)>([&records, clobberedRegisters = registersClobberedByThrowSites(catalog)](B3::Procedure& proc) {
            return throwWhereTrapIsCertain(proc, records, clobberedRegisters);
        })
    };
}

void ThrowEmitter::requireInitialized(const OperationContext& context, Operand operand, const Type& type, StringID name)
{
    if (!(type.primitives & Primitive::Absent))
        return;
    ThrowSite site { ThrowKind::UninitializedBinding, nameWord(name), std::nullopt };
    if (operand.representation == Representation::Void) {
        throwAt(context, site, std::nullopt);
        return;
    }
    requireOrThrow(context, logicalNot(emitter<ConversionEmitter>().isAbsent(operand)), site, std::nullopt);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
