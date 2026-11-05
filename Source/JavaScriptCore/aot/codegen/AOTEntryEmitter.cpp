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
