/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTEntryEmitter.h"

#if ENABLE(AOT)

#include "AOTArgumentLocations.h"
#include "AOTClosureWord.h"
#include "AOTCodeRecords.h"
#include "AOTConvention.h"
#include "AOTIRProgram.h"
#include "AOTRepresentationRules.h"
#include "AOTStubsCallFrames.h"
#include "AOTThrowEmitter.h"
#include "AOTTypeTestPlan.h"
#include "AOTVector.h"
#include "AllowMacroScratchRegisterUsage.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static std::optional<uint32_t> firstArgumentInMemory(EntryConvention convention)
{
    switch (convention) {
    case EntryConvention::Direct:
        return std::nullopt;
    case EntryConvention::GenericRegisters:
        return numberOfArgumentGPRs;
    case EntryConvention::GenericBuffer:
        return 0;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static uint8_t numberOfRegisterParameters(const CodeGenerationInputs& inputs, CodeUnit unit)
{
    const CallingConventions& conventions = inputs.plan.conventions();
    const Function& function = inputs.program.at(unit.function);

    size_t numberOfArguments = 0;
    switch (conventions.entryConventionOf(unit)) {
    case EntryConvention::Direct:
        return 0;
    case EntryConvention::GenericBuffer:
        numberOfArguments = function.length;
        break;
    case EntryConvention::GenericRegisters:
        numberOfArguments = function.numberOfPositionalParameters();
        break;
    }
    numberOfArguments += conventions.genericEntry(unit).takesNewTarget;
    return static_cast<uint8_t>(std::min<size_t>(numberOfArguments, numberOfArgumentGPRs));
}

void countEntryIntoCode(Machine::PatchOperation* count)
{
    count->effects = Machine::Effects::none();
    count->effects.writesLocalState = true;
    count->setRunsWithoutFrame(true);
    count->clobber(RegisterSet::macroClobberedGPRs());
    count->setGenerator([](CCallHelpers& jit, const Machine::PatchParameters&) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        jit.add64(CCallHelpers::TrustedImm32(1), CCallHelpers::Address(instanceGPR, AOT_INSTANCE_OFFSET(effectsNet.entriesIntoCode)));
    });
}

void EntryEmitter::generateEntries()
{
    generateEntry(0, conventions().locationsOf(m_program, m_unit));
}

void EntryEmitter::generateEntry(uint16_t entry, const ArgumentLocations& locations)
{
    BlockID block = m_code.entries()[entry].block;
    std::span<const ValueID> parameters = m_code.at(block).parameters();
    RELEASE_ASSERT(locations.parameters.size() == parameters.size());

    m_out.appendTo(m_proc.addEntrypoint(entry));
    if (std::optional<uint32_t> firstArgument = firstArgumentInMemory(locations.convention)) {
        m_argumentCount = Word32(m_out.castToInt32(m_out.argumentRegister(countGPR)));
        m_argumentList = offsetBy(Pointer(m_out.framePointer()), offsetOfStackWordsFromFrame);
        m_indexOfFirstArgumentInList = *firstArgument;
    }

    Vector<Operand, 8> passed(parameters.size());
    for (bool readsStackWords : { false, true }) {
        for (size_t index = 0; index < parameters.size(); ++index) {
            const ArgumentLocation& location = locations.parameters[index];
            if (location.kind != ArgumentLocation::Kind::None && m_code.isNeeded(parameters[index]) && (location.kind == ArgumentLocation::Kind::Stack) == readsStackWords)
                passed[index] = read(location, locations.convention);
        }
    }
    const bool goesOnHere = generator().goesOnInBlockOfEntry(block);
    for (size_t index = 0; index < parameters.size(); ++index) {
        if (goesOnHere)
            generator().defineParameterAsPassed(parameters[index], passed[index]);
        else
            generator().defineParameter(parameters[index], passed[index]);
    }
    if (AOT::hasFlag(build().options().testing, Wire::TestingSwitch::CheckEffectsAtRunTime)) [[unlikely]]
        countEntryIntoCode(m_out.patchpoint(Machine::Void));
    if (goesOnHere) {
        generator().goOnInBlockOfEntry(block);
        return;
    }
    m_out.jump(generator().blockFor(block));
}

Operand EntryEmitter::read(const ArgumentLocation& location, EntryConvention convention)
{
    switch (location.kind) {
    case ArgumentLocation::Kind::None:
        break;
    case ArgumentLocation::Kind::GPR: {
        LValue value = m_out.argumentRegister(location.reg.gpr());
        if (machineTypeOf(location.representation) == Machine::Int32)
            value = m_out.castToInt32(value);
        return Operand(location.representation, value);
    }
    case ArgumentLocation::Kind::FPR: {
        LValue value = m_out.argumentRegister(location.reg.fpr());
        return Operand(location.representation, value);
    }
    case ArgumentLocation::Kind::Stack: {
        auto loadStackWord = [&] {
            TypedPointer word = m_out.address(m_heaps.argumentBuffer, m_out.framePointer(), offsetOfStackWordFromFrame(location.stackIndex));
            return m_out.load(word, machineTypeOf(location.representation));
        };
        switch (convention) {
        case EntryConvention::Direct:
            return Operand(location.representation, loadStackWord());
        case EntryConvention::GenericRegisters:
            RELEASE_ASSERT(location.representation == Representation::JSValue);
            return either<Boxed>(below(word32(numberOfArgumentGPRs + location.stackIndex), m_argumentCount), [&] {
                return Boxed(loadStackWord());
            }, [&] {
                return undefined();
            });
        case EntryConvention::GenericBuffer:
            break;
        }
        break;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Word32 EntryEmitter::argumentCount() const
{
    RELEASE_ASSERT(m_argumentCount.value());
    return m_argumentCount;
}

Pointer EntryEmitter::argumentList() const
{
    RELEASE_ASSERT(m_argumentCount.value());
    return m_argumentList;
}

uint32_t EntryEmitter::indexOfFirstArgumentInList() const
{
    RELEASE_ASSERT(m_argumentCount.value());
    return m_indexOfFirstArgumentInList;
}

uint32_t EntryEmitter::numberOfArgumentsAlwaysInList() const
{
    RELEASE_ASSERT(m_argumentCount.value() && !m_indexOfFirstArgumentInList);
    return numberOfRegisterParameters(inputs(), m_unit);
}

static uint32_t stackWordsPassedBy(const CodeGenerationInputs& inputs, const Operation& tailCall)
{
    const CallingConventions& conventions = inputs.plan.conventions();
    if (tailCall.opcode == Opcode::CallDirect)
        return conventions.stackWordsReservedByCallersOf(inputs.program, conventions.unitEnteredBy(tailCall));
    if (tailCall.opcode == Opcode::CallTable)
        return conventions.stackWordsReservedByCallersOf(inputs.program, conventions.tableLookupOf(conventions.methodSelector(tailCall.selector())));
    if (tailCall.opcode != Opcode::Call)
        return 0;
    size_t numberOfArguments = tailCall.argumentsOperands().size();
    return numberOfArguments > numberOfArgumentGPRs ? static_cast<uint32_t>(numberOfArguments - numberOfArgumentGPRs) : 0;
}

static bool passesStackWordsInTailCall(const CodeGenerationInputs& inputs, const Code& code)
{
    if (!code.mayHoldReturnedCalls())
        return false;
    for (uint32_t index = 0; index < code.numberOfBlocks(); ++index) {
        BlockID block(index);
        const Terminator& terminator = code.at(block).terminator;
        if (terminator.kind == TerminatorKind::ReturnCall && code.isReached(block) && stackWordsPassedBy(inputs, code.at(terminator.callOperand())))
            return true;
    }
    return false;
}

bool readsCallerFrame(const CodeGenerationInputs& inputs, const Code& code)
{
    const CallingConventions& conventions = inputs.plan.conventions();
    CodeUnit unit = code.unit();
    if (conventions.entryConventionOf(unit) != EntryConvention::Direct)
        return true;
    if (conventions.stackWordsReservedByCallersOf(inputs.program, unit))
        return true;
    return passesStackWordsInTailCall(inputs, code);
}

static uint8_t argumentsBorrowedThrough(const CodeGenerationInputs& inputs, CodeUnit unit)
{
    return unit.role == CodeRole::GenericCallEntry ? inputs.plan.conventions().genericEntry(unit).borrowedArguments : 0;
}

static bool rowOfTailCallsThroughEntryEnds(const CodeGenerationInputs& inputs, CodeUnit unit)
{
    return unit.role == CodeRole::GenericCallEntry && inputs.plan.endsItsTailCalls(unit.function);
}

EntryDescription describeEntry(const CodeGenerationInputs& inputs, CodeUnit unit)
{
    return {
        .numberOfParameters = numberOfRegisterParameters(inputs, unit),
        .borrowedArguments = argumentsBorrowedThrough(inputs, unit),
        .endsItsTailCalls = rowOfTailCallsThroughEntryEnds(inputs, unit),
        .isGenericEntrypoint = isGenericEntry(unit.role),
    };
}

static void emitArgumentRegisterSpill(CCallHelpers& jit, CodeRecords& records)
{
    AllowMacroScratchRegisterUsage allowScratch(jit);
    records.entries.append(RecordedEntry { primaryEntry, jit.label() });
    emitSpillArgumentRegisters(jit);
    records.frame.stackUsedAtCalls += sizeOfSpilledArgumentRegisters;
    records.frame.spillsArgumentRegisters = true;
}

namespace {

struct PendingCallEntryMove {
    enum class Kind : uint8_t { Register, Load, Undefined };

    bool reads(Reg reg) const { return kind != Kind::Undefined && Reg(source) == reg; }

    Kind kind { Kind::Undefined };
    GPRReg source { InvalidGPRReg };
    int32_t offset { 0 };
    ArgumentLocation destination;
};

} // anonymous namespace

static void emitCallEntryMove(CCallHelpers& jit, const PendingCallEntryMove& move)
{
    switch (move.kind) {
    case PendingCallEntryMove::Kind::Register:
        jit.move(move.source, move.destination.reg.gpr());
        return;
    case PendingCallEntryMove::Kind::Undefined:
        jit.move(CCallHelpers::TrustedImm64(JSValue::ValueUndefined), move.destination.reg.gpr());
        return;
    case PendingCallEntryMove::Kind::Load: {
        CCallHelpers::Address word(move.source, move.offset);
        if (move.destination.kind == ArgumentLocation::Kind::FPR)
            jit.loadDouble(word, move.destination.reg.fpr());
        else if (machineTypeOf(move.destination.representation) == Machine::Int32)
            jit.load32(word, move.destination.reg.gpr());
        else
            jit.load64(word, move.destination.reg.gpr());
        return;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool emitFusedCallEntry(CCallHelpers& jit, const CodeGenerationInputs& inputs, FunctionID function)
{
#if CPU(X86_64)
    constexpr GPRReg scratch = MacroAssembler::s_scratchRegister;
#elif CPU(ARM64)
    constexpr GPRReg scratch = MacroAssembler::dataTempRegister;
#endif
    const CallingConventions& conventions = inputs.plan.conventions();
    std::optional<Vector<CallEntryMove, 8>> planned = planCallEntryMoves(inputs.program, conventions, function);
    RELEASE_ASSERT(planned);
    std::optional<ptrdiff_t> offsetOfCaptures;

    Vector<PendingCallEntryMove, 8> pending;
    for (const CallEntryMove& move : *planned) {
        RELEASE_ASSERT(move.destination.kind == ArgumentLocation::Kind::GPR || (move.destination.kind == ArgumentLocation::Kind::FPR && move.source == CallEntryMove::Source::Capture));
        RELEASE_ASSERT(move.destination.reg != Reg(scratch));
        PendingCallEntryMove next { PendingCallEntryMove::Kind::Register, InvalidGPRReg, 0, move.destination };
        switch (move.source) {
        case CallEntryMove::Source::This:
            next.source = thisGPR;
            break;
        case CallEntryMove::Source::Closure:
            next.source = calleeGPR;
            break;
        case CallEntryMove::Source::Argument:
            next.source = argumentGPR(move.index);
            break;
        case CallEntryMove::Source::Capture:
            if (!offsetOfCaptures)
                offsetOfCaptures = inputs.plan.functionKind(inputs.program, conventions.closureEntryOf(function)).isSlim ? offsetOfSlimClosureCaptures : offsetOfClosureCaptures;
            next.kind = PendingCallEntryMove::Kind::Load;
            next.source = calleeGPR;
            next.offset = static_cast<int32_t>(*offsetOfCaptures + static_cast<ptrdiff_t>(move.index * sizeof(EncodedJSValue)));
            break;
        case CallEntryMove::Source::Undefined:
            next.kind = PendingCallEntryMove::Kind::Undefined;
            break;
        }
        if (next.kind != PendingCallEntryMove::Kind::Register || !next.reads(next.destination.reg))
            pending.append(next);
    }
    if (pending.isEmpty())
        return false;

    while (!pending.isEmpty()) {
        size_t ready = pending.findIf([&](const PendingCallEntryMove& move) {
            return std::ranges::none_of(pending, [&](const PendingCallEntryMove& other) {
                return &other != &move && other.reads(move.destination.reg);
            });
        });
        if (ready != notFound) {
            emitCallEntryMove(jit, pending[ready]);
            pending.removeAt(ready);
            continue;
        }
        Reg overwritten = pending[0].destination.reg;
        for (PendingCallEntryMove& move : pending) {
            RELEASE_ASSERT(!move.reads(Reg(scratch)));
            if (move.reads(overwritten))
                move.source = scratch;
        }
        jit.move(overwritten.gpr(), scratch);
    }
    return true;
}

static std::optional<QuestionOfEntry> findSimpleQuestionOfEntry(const CodeGenerationInputs& inputs, TypeArena& types, GPRReg value, const TypeTestPlan& plan)
{
    uint16_t numbers = plan.passingPrimitives & Primitive::Number;
    if (plan.integers || !plan.stringLiterals.isEmpty() || (numbers && numbers != Primitive::Number))
        return std::nullopt;

    QuestionOfEntry question { .value = value, .passingPrimitives = plan.passingPrimitives, .everyObjectPasses = plan.objects == ObjectsVerdict::AllPass };
    for (const CellTest& test : plan.cellTests) {
        switch (test.kind) {
        case CellTestKind::IsObject:
            question.everyObjectPasses = true;
            break;
        case CellTestKind::CellTypeIs:
            question.cellTypes.append(test.cellTypes);
            break;
        case CellTestKind::ShapeIn: {
            std::span<const LayoutID> members = membersOf(types, test.layouts);
            ShapeTest shapes = inputs.plan.shapes().testOf(test.layouts, members);
            if (shapes.form == ShapeTestForm::NeverPasses)
                break;
            if (test.asksForInstance || question.shapeWord || shapes.form != ShapeTestForm::OneShape)
                return std::nullopt;
            question.shapeWord = AOT::shapeWordOf(shapes.first);
            question.shapeMayBearMark = std::ranges::any_of(members, [&](LayoutID member) {
                return inputs.plan.layout(member).mayBearMark;
            });
            break;
        }
        default:
            return std::nullopt;
        }
    }
    return question;
}

std::optional<Vector<QuestionOfEntry, 4>> findQuestionsOfEntryWrittenByHand(const CodeGenerationInputs& inputs, TypeArena& types, CodeUnit unit, NOESCAPE const ScopedLambda<TypeID(unsigned)>& typeAskedOfEntryParameter)
{
    const CallingConventions& conventions = inputs.plan.conventions();
    if (unit.role != CodeRole::GenericCallEntry || conventions.hasFusedCallEntry(unit.function) || conventions.spillsArgumentRegisters(unit) || !planCallEntryMoves(inputs.program, conventions, unit.function))
        return std::nullopt;

    const Function& function = inputs.program.at(unit.function);
    ArgumentLocations taken = conventions.locationsOf(inputs.program, conventions.genericEntry(unit).target);
    Vector<QuestionOfEntry, 4> questions;
    for (unsigned index = 0; index < function.numberOfEntryParameters(); ++index) {
        EntryParameter kind = function.entryParameterKind(index);
        if (kind == EntryParameter::NewTarget && taken.parameters[index].kind != ArgumentLocation::Kind::None) {
            TypeID believed = typeAskedOfEntryParameter(index);
            if (believed && !(types.at(believed).primitives & Primitive::Undefined))
                return std::nullopt;
        }
        if (kind == EntryParameter::Capture && taken.parameters[index].kind != ArgumentLocation::Kind::None && inputs.facts.linked().bodyTakesWhatCallersLeave(unit.function) && typeAskedOfEntryParameter(index))
            return std::nullopt;
        if ((kind != EntryParameter::This && kind != EntryParameter::Parameter) || taken.parameters[index].kind == ArgumentLocation::Kind::None)
            continue;
        TypeID asked = typeAskedOfEntryParameter(index);
        if (!asked)
            continue;
        TypeTestPlan plan = computeTypeTestPlan(types, TypeTestContext { inputs.plan, inputs.plan.hasOneInstance() }, TypeTable::anything, Representation::JSValue, asked);
        if (plan.verdict == TypeTestVerdict::AlwaysPasses)
            continue;
        std::optional<QuestionOfEntry> question = findSimpleQuestionOfEntry(inputs, types, kind == EntryParameter::This ? thisGPR : argumentGPR(index - function.firstParameterIndex()), plan);
        if (!question)
            return std::nullopt;
        questions.append(WTF::move(*question));
    }
    return questions;
}

static constexpr GPRReg scratchOfEntryWrittenByHand = valueEntryGPR;

enum class CaseOfQuestionOfEntry : uint8_t { Number, Text, Symbol, BigInteger, Object, CellType, Shape, Boolean, False, True, Undefined, Null };

struct CaseInQuestionOfEntry {
    CaseOfQuestionOfEntry which;
    unsigned index { 0 };
};

static Vector<CaseInQuestionOfEntry, 8> cellsThatFitQuestionOfEntry(const QuestionOfEntry& question)
{
    Vector<CaseInQuestionOfEntry, 8> cases;
    if (question.shapeWord)
        cases.append({ CaseOfQuestionOfEntry::Shape });
    if (question.everyObjectPasses)
        cases.append({ CaseOfQuestionOfEntry::Object });
    for (unsigned index = 0; index < question.cellTypes.size(); ++index)
        cases.append({ CaseOfQuestionOfEntry::CellType, index });
    if (question.passingPrimitives & Primitive::AnyString)
        cases.append({ CaseOfQuestionOfEntry::Text });
    if (question.passingPrimitives & Primitive::AnySymbol)
        cases.append({ CaseOfQuestionOfEntry::Symbol });
    if (question.passingPrimitives & Primitive::BigInt)
        cases.append({ CaseOfQuestionOfEntry::BigInteger });
    return cases;
}

static Vector<CaseInQuestionOfEntry, 8> wordsThatFitQuestionOfEntry(const QuestionOfEntry& question)
{
    Vector<CaseInQuestionOfEntry, 8> cases;
    uint16_t passing = question.passingPrimitives;
    if ((passing & Primitive::Boolean) == Primitive::Boolean)
        cases.append({ CaseOfQuestionOfEntry::Boolean });
    else if (passing & Primitive::Boolean)
        cases.append({ passing & Primitive::True ? CaseOfQuestionOfEntry::True : CaseOfQuestionOfEntry::False });
    if (passing & Primitive::Undefined)
        cases.append({ CaseOfQuestionOfEntry::Undefined });
    if (passing & Primitive::Null)
        cases.append({ CaseOfQuestionOfEntry::Null });
    return cases;
}

static CCallHelpers::Jump branchOnCaseOfQuestionOfEntry(CCallHelpers& jit, const QuestionOfEntry& question, GPRReg scratch, CaseInQuestionOfEntry asked, bool whereItFits)
{
    GPRReg value = question.value;
    CCallHelpers::RelationalCondition condition = whereItFits ? CCallHelpers::Equal : CCallHelpers::NotEqual;
    auto isWord = [&](JSValue word) {
        return jit.branch64(condition, value, CCallHelpers::TrustedImm32(static_cast<int32_t>(JSValue::encode(word))));
    };
    switch (asked.which) {
    case CaseOfQuestionOfEntry::Number:
        return whereItFits ? jit.branchIfNumber(value) : jit.branchIfNotNumber(value);
    case CaseOfQuestionOfEntry::Text:
        return whereItFits ? jit.branchIfString(value) : jit.branchIfNotString(value);
    case CaseOfQuestionOfEntry::Symbol:
        return whereItFits ? jit.branchIfSymbol(value) : jit.branchIfNotSymbol(value);
    case CaseOfQuestionOfEntry::BigInteger:
        return whereItFits ? jit.branchIfHeapBigInt(value) : jit.branchIfNotHeapBigInt(value);
    case CaseOfQuestionOfEntry::Object:
        return whereItFits ? jit.branchIfObject(value) : jit.branchIfNotObject(value);
    case CaseOfQuestionOfEntry::CellType:
        return whereItFits ? jit.branchIfType(value, question.cellTypes[asked.index]) : jit.branchIfNotType(value, question.cellTypes[asked.index]);
    case CaseOfQuestionOfEntry::Shape:
        if (!question.shapeMayBearMark)
            return jit.branch32(condition, CCallHelpers::Address(value, offsetOfShapeWord), CCallHelpers::TrustedImm32(static_cast<int32_t>(*question.shapeWord)));
        jit.load32(CCallHelpers::Address(value, offsetOfShapeWord), scratch);
        jit.or32(CCallHelpers::TrustedImm32(static_cast<int32_t>(shapeWordIsUnmarked)), scratch);
        return jit.branch32(condition, scratch, CCallHelpers::TrustedImm32(static_cast<int32_t>(*question.shapeWord)));
    case CaseOfQuestionOfEntry::Boolean:
        return whereItFits ? jit.branchIfBoolean(value, scratch) : jit.branchIfNotBoolean(value, scratch);
    case CaseOfQuestionOfEntry::False:
        return isWord(jsBoolean(false));
    case CaseOfQuestionOfEntry::True:
        return isWord(jsBoolean(true));
    case CaseOfQuestionOfEntry::Undefined:
        return isWord(jsUndefined());
    case CaseOfQuestionOfEntry::Null:
        return isWord(jsNull());
    }
    RELEASE_ASSERT_NOT_REACHED();
}

struct WaysOutOfQuestionOfEntry {
    CCallHelpers::JumpList misfits;
    CCallHelpers::JumpList isNoCell;
    CCallHelpers::Label behind;
};

static void emitQuestionOfEntry(CCallHelpers& jit, const QuestionOfEntry& question, GPRReg scratch, WaysOutOfQuestionOfEntry& ways)
{
    CCallHelpers::JumpList fits;
    Vector<CaseInQuestionOfEntry, 8> inLine = cellsThatFitQuestionOfEntry(question);
    bool asksForNumbers = question.passingPrimitives & Primitive::Number;
    if (inLine.isEmpty()) {
        if (asksForNumbers)
            inLine.append({ CaseOfQuestionOfEntry::Number });
        inLine.appendVector(wordsThatFitQuestionOfEntry(question));
    } else {
        if (asksForNumbers)
            fits.append(branchOnCaseOfQuestionOfEntry(jit, question, scratch, { CaseOfQuestionOfEntry::Number }, true));
        (wordsThatFitQuestionOfEntry(question).isEmpty() ? ways.misfits : ways.isNoCell).append(jit.branchIfNotCell(question.value));
    }
    if (inLine.isEmpty())
        ways.misfits.append(jit.jump());
    for (size_t index = 0; index < inLine.size(); ++index) {
        bool isLast = index + 1 == inLine.size();
        (isLast ? ways.misfits : fits).append(branchOnCaseOfQuestionOfEntry(jit, question, scratch, inLine[index], !isLast));
    }
    fits.link(&jit);
    ways.behind = jit.label();
}

CCallHelpers::JumpList emitQuestionOfEntryWrittenByHand(CCallHelpers& jit, const QuestionOfEntry& question)
{
    AllowMacroScratchRegisterUsage allowScratch(jit);
    WaysOutOfQuestionOfEntry ways;
    emitQuestionOfEntry(jit, question, scratchOfEntryWrittenByHand, ways);
    if (ways.isNoCell.empty())
        return ways.misfits;

    CCallHelpers::Jump fits = jit.jump();
    ways.isNoCell.link(&jit);
    for (CaseInQuestionOfEntry word : wordsThatFitQuestionOfEntry(question))
        branchOnCaseOfQuestionOfEntry(jit, question, scratchOfEntryWrittenByHand, word, true).linkTo(ways.behind, &jit);
    ways.misfits.append(jit.jump());
    fits.link(&jit);
    return ways.misfits;
}

void emitEntryWrittenByHand(CCallHelpers& jit, const CodeGenerationInputs& inputs, CodeRecords& records, CodeUnit unit, std::span<const QuestionOfEntry> questions, bool frameIsSetUp, bool nobodyEnters)
{
    AllowMacroScratchRegisterUsage allowScratch(jit);
    if (nobodyEnters) {
        uint32_t site = records.sites.addSite({ .origin = Origin { .position = inputs.program.at(unit.function).position, .inlineFrame = 0, .isTailCall = false, .isPartOfConstruction = false } });
        if (!frameIsSetUp)
            jit.emitFunctionPrologue();
        emitStraightThrowSite(jit, records, ThrowSite { .kind = ThrowKind::ErrorWithMessage, .a = ThrowSiteWord::none, .b = std::nullopt, .sourceOfFalseProof = ProofSource::EntryParameter }, std::nullopt, 0, site);
        jit.breakpoint();
        return;
    }
    constexpr GPRReg scratch = scratchOfEntryWrittenByHand;
    Vector<WaysOutOfQuestionOfEntry, 4> ways(questions.size());
    for (size_t index = 0; index < questions.size(); ++index)
        emitQuestionOfEntry(jit, questions[index], scratch, ways[index]);

    emitFusedCallEntry(jit, inputs, unit.function);
    if (frameIsSetUp)
        jit.emitFunctionEpilogue();
    records.stubCalls.jumpToFunction(jit, inputs.plan.conventions().genericEntry(unit).target, primaryEntry);
    if (questions.empty())
        return;

    uint32_t site = records.sites.addSite({ .origin = Origin { .position = inputs.program.at(unit.function).position, .inlineFrame = 0, .isTailCall = false, .isPartOfConstruction = false } });
    for (size_t index = 0; index < questions.size(); ++index) {
        if (!ways[index].isNoCell.empty()) {
            ways[index].isNoCell.link(&jit);
            for (CaseInQuestionOfEntry word : wordsThatFitQuestionOfEntry(questions[index]))
                branchOnCaseOfQuestionOfEntry(jit, questions[index], scratch, word, true).linkTo(ways[index].behind, &jit);
        }
        ways[index].misfits.link(&jit);
        if (!frameIsSetUp)
            jit.emitFunctionPrologue();
        emitStraightThrowSite(jit, records, ThrowSite { .kind = ThrowKind::ErrorWithMessage, .a = ThrowSiteWord::none, .b = std::nullopt, .sourceOfFalseProof = ProofSource::EntryParameter, .expectedProofType = FalseProofWord::summarize(questions[index].passingPrimitives, questions[index].everyObjectPasses || questions[index].shapeWord || !questions[index].cellTypes.isEmpty()) }, questions[index].value, 0, site);
        jit.breakpoint();
    }
}

CCallHelpers::Jump emitEntryPreambles(CCallHelpers& jit, const CodeGenerationInputs& inputs, CodeUnit unit, CodeRecords& records)
{
    const CallingConventions& conventions = inputs.plan.conventions();
    bool spillsArgumentRegisters = conventions.spillsArgumentRegisters(unit);

    bool holdsFusedCallEntry = conventions.holdsFusedCallEntry(unit);
    RELEASE_ASSERT(!holdsFusedCallEntry || !spillsArgumentRegisters);
    CCallHelpers::Label startOfFusedCallEntry = jit.label();
    bool fusedCallEntryMoves = holdsFusedCallEntry && emitFusedCallEntry(jit, inputs, unit.function);
    bool primaryEntryFollows = unit.role != CodeRole::ParameterGuard;
    CCallHelpers::Jump toPrimaryEntry;
    if (fusedCallEntryMoves) {
        records.entries.append(RecordedEntry { fusedCallEntry, startOfFusedCallEntry });
        if (!primaryEntryFollows)
            toPrimaryEntry = jit.jump();
    }

    if (spillsArgumentRegisters)
        emitArgumentRegisterSpill(jit, records);

    if (!primaryEntryFollows)
        return toPrimaryEntry;
    if (toPrimaryEntry.isSet())
        toPrimaryEntry.link(&jit);
    if (holdsFusedCallEntry && !fusedCallEntryMoves)
        records.entries.append(RecordedEntry { fusedCallEntry, jit.label() });
    return { };
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
