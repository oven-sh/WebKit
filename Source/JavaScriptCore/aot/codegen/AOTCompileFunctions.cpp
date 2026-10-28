/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCompileFunctions.h"

#if ENABLE(AOT)

#include "AOTBranchCounterNumbers.h"
#include "AOTCellFormat.h"
#include "AOTChooseCallingConventions.h"
#include "AOTCodeDescription.h"
#include "AOTCodeSizeEstimate.h"
#include "AOTCurrentWork.h"
#include "AOTElideStackChecks.h"
#include "AOTFunctionCodeGenerator.h"
#include "AOTFunctionKinds.h"
#include "AOTImageBuilder.h"
#include "AOTPlacement.h"
#include "AOTSort.h"
#include "AOTStubAssembler.h"
#include "AOTVector.h"
#include "B3Generate.h"
#include <algorithm>
#include <array>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct RegisterTransfer {
    Reg destination;
    Reg source;
    std::optional<int32_t> offsetFromSource;
    bool isHalfWord { false };
};

struct ForwardingPlan {
    FunctionID callee;
    Vector<RegisterTransfer, 8> transfers;
};

struct ForwardedArgument {
    RegisterTransfer transfer;
    uint32_t order;
    bool isMovedFirst { false };
};

class RegisterContents {
public:
    RegisterContents()
    {
        for (unsigned index = 0; index < m_contents.size(); ++index)
            m_contents[index] = index;
    }

    static uint32_t atEntry(Reg reg) { return reg.index(); }
    static uint32_t loaded(const RegisterTransfer& load) { return static_cast<uint32_t>(Reg::maxIndex() + 1) + static_cast<uint32_t>(*load.offsetFromSource); }
    static uint32_t broughtBy(const RegisterTransfer& transfer) { return transfer.offsetFromSource ? loaded(transfer) : atEntry(transfer.source); }

    uint32_t of(Reg reg) const { return m_contents[reg.index()]; }

    bool apply(const RegisterTransfer& transfer)
    {
        if (!transfer.offsetFromSource) {
            m_contents[transfer.destination.index()] = of(transfer.source);
            return true;
        }
        if (of(transfer.source) != atEntry(transfer.source))
            return false;
        m_contents[transfer.destination.index()] = loaded(transfer);
        return true;
    }

private:
    std::array<uint32_t, Reg::maxIndex() + 1> m_contents;
};

} // anonymous namespace

static bool generatesOnlyWhatTheProgramAsksFor(const CodeGenerationWorker& worker, const CodeGenerationInputs& inputs, FunctionID id)
{
#if AOT_CHECKS_ASAN_SHADOW_IN_COMPILED_CODE
    UNUSED_PARAM(worker);
    UNUSED_PARAM(inputs);
    UNUSED_PARAM(id);
    return false;
#else
    BuildContext& build = worker.build;
    const AOT::CompileOptions& options = build.options();
    if (build.validates() || options.validatesReferences || options.writesCodeSizeMap())
        return false;
    if ((options.logging.dumpsB3Graph || options.logging.dumpsDisassembly) && build.shouldDump(id))
        return false;
    return !inputs.branchCounters.counterOfEntries(id);
#endif
}

static ValueID valueBehindCasts(const Function& function, ValueID id)
{
    while (function.at(id).opcode == Opcode::AssumeType || isCastThatProvesNothing(function.at(id).opcode))
        id = function.at(id).valueOperand();
    return id;
}

static bool returnsWhatCalleeReturns(const ArgumentLocations& own, const ArgumentLocations& callee)
{
    bool returnsNothing = std::ranges::all_of(own.results, [](const ArgumentLocation& result) {
        return result.kind == ArgumentLocation::Kind::None;
    });
    return returnsNothing || std::ranges::equal(callee.results, own.results, [](const ArgumentLocation& returned, const ArgumentLocation& expected) {
        return returned.kind == expected.kind && returned.reg == expected.reg && returned.stackIndex == expected.stackIndex
            && haveSameRegisterRepresentation(returned.representation, expected.representation);
    });
}

static std::optional<RegisterTransfer> findTransfer(const CodeGenerationInputs& inputs, const Function& function, const ArgumentLocations& own, ValueID argument, const ArgumentLocation& destination)
{
    const Block& block = function.at(entryBlock);
    auto locationOfParameter = [&](ValueID parameter) -> const ArgumentLocation* {
        size_t index = block.parameters.find(parameter);
        return index == notFound ? nullptr : &own.parameters[index];
    };

    const Value& value = function.at(argument);
    if (value.opcode == Opcode::GetCapture) {
        const ArgumentLocation* closure = locationOfParameter(valueBehindCasts(function, value.closureOperand()));
        if (!closure || closure->kind != ArgumentLocation::Kind::GPR || closure->representation != Representation::Cell || value.representation != destination.representation)
            return std::nullopt;
        if (value.representation == Representation::Cell32)
            return std::nullopt;
        const Function& owner = inputs.program.at(value.function());
        ptrdiff_t firstCapture = inputs.placement.functionKind(value.function()).isSlim ? offsetOfSlimClosureCaptures : offsetOfClosureCaptures;
        ptrdiff_t offset = firstCapture + static_cast<ptrdiff_t>(closureCaptureSlotOf(inputs.program, owner, value.index()) * sizeof(EncodedJSValue));
        return RegisterTransfer { destination.reg, closure->reg, safeCast<int32_t>(offset), b3TypeOf(value.representation) == B3::Int32 };
    }

    const ArgumentLocation* source = locationOfParameter(argument);
    if (!source || source->kind != destination.kind || source->representation != destination.representation)
        return std::nullopt;
    if (source->kind == ArgumentLocation::Kind::FPR ? source->reg != destination.reg : b3TypeOf(source->representation) != B3::Int64)
        return std::nullopt;
    return RegisterTransfer { destination.reg, source->reg, std::nullopt };
}

static void chooseArgumentsMovedFirst(std::span<ForwardedArgument> moves, std::span<const ForwardedArgument> loads)
{
    auto isOverwrittenBefore = [&](size_t position) {
        Reg source = moves[position].transfer.source;
        for (const ForwardedArgument& load : loads) {
            if (load.transfer.destination == source)
                return true;
        }
        for (size_t other = 0; other < moves.size(); ++other) {
            if (moves[other].transfer.destination == source && (moves[other].isMovedFirst || other < position))
                return true;
        }
        return false;
    };
    for (bool changed = true; changed;) {
        changed = false;
        for (size_t position = 0; position < moves.size(); ++position) {
            if (moves[position].isMovedFirst || !isOverwrittenBefore(position))
                continue;
            moves[position].isMovedFirst = true;
            changed = true;
        }
    }
}

static std::optional<ForwardingPlan> planForwarding(const CodeGenerationWorker& worker, const CodeGenerationInputs& inputs, FunctionID id)
{
    const Program& program = inputs.program;
    const Function& function = program.at(id);
    if (function.blocks.size() != 1 || function.inlineFrames.size() > 1 || function.role == FunctionRole::StubBody || function.role == FunctionRole::CoroutineBody)
        return std::nullopt;
    const Block& block = function.at(entryBlock);
    if (block.terminator.kind != TerminatorKind::ReturnCall || block.handler || block.values.isEmpty())
        return std::nullopt;
    const Value& call = function.at(block.values.last());
    if (call.opcode != Opcode::CallDirect || !program.at(call.function()).hasBody() || program.typeOf(function, block.values.last()).isNever())
        return std::nullopt;
    std::span<const ValueID> valuesBeforeCall = block.values.span().first(block.values.size() - 1);
    for (ValueID value : valuesBeforeCall) {
        Opcode opcode = function.at(value).opcode;
        if (opcode != Opcode::GetCapture && opcode != Opcode::AssumeType && !makesFact(opcode) && !isCastThatProvesNothing(opcode))
            return std::nullopt;
    }
    if (!generatesOnlyWhatTheProgramAsksFor(worker, inputs, id))
        return std::nullopt;

    const CallingConventions& conventions = inputs.conventions;
    if (inputs.stackChecks.of(id) == StackCheck::Required || !conventions.secondaryEntriesOf(id).empty() || conventions.of(program, id).spillsArgumentRegisters)
        return std::nullopt;
    ArgumentLocations own = conventions.locationsOf(program, id);
    ArgumentLocations callee = conventions.locationsOf(program, call.function());
    if (own.convention == EntryConvention::GenericBuffer || callee.convention != EntryConvention::Direct)
        return std::nullopt;
    if (callee.numberOfStackArguments || conventions.stackWordsReservedByCallersOf(program, call.function(), callee) || !returnsWhatCalleeReturns(own, callee))
        return std::nullopt;
    bool hasParameterInMemory = std::ranges::any_of(own.parameters, [](const ArgumentLocation& location) {
        return location.kind == ArgumentLocation::Kind::Stack;
    });
    if (hasParameterInMemory)
        return std::nullopt;

    Vector<ForwardedArgument, 8> moves;
    Vector<ForwardedArgument, 8> loads;
    Vector<RegisterTransfer, 8> expected;
    std::span<const ValueID> arguments = call.argumentsOperands();
    RELEASE_ASSERT(arguments.size() == callee.parameters.size());
    for (size_t index = 0; index < arguments.size(); ++index) {
        const ArgumentLocation& destination = callee.parameters[index];
        if (destination.kind == ArgumentLocation::Kind::None)
            continue;
        ValueID argument = valueBehindCasts(function, arguments[index]);
        std::optional<RegisterTransfer> transfer = findTransfer(inputs, function, own, argument, destination);
        if (!transfer)
            return std::nullopt;
        bool isPassedTwice = expected.containsIf([&](const RegisterTransfer& other) {
            return other.source == transfer->source && other.offsetFromSource == transfer->offsetFromSource;
        });
        if (isPassedTwice)
            return std::nullopt;
        expected.append(*transfer);
        if (transfer->offsetFromSource) {
            size_t position = valuesBeforeCall.size();
            for (size_t candidate = 0; candidate < valuesBeforeCall.size(); ++candidate) {
                if (valuesBeforeCall[candidate] == argument)
                    position = candidate;
            }
            loads.append({ *transfer, static_cast<uint32_t>(position) });
        } else if (transfer->source != transfer->destination)
            moves.append({ *transfer, static_cast<uint32_t>(block.parameters.find(argument)) });
    }

    AOT::sort(loads, { }, &ForwardedArgument::order);
    chooseArgumentsMovedFirst(moves.mutableSpan(), loads.span());

    ForwardingPlan plan { call.function(), { } };
    Vector<ForwardedArgument, 8> movedFirst;
    for (const ForwardedArgument& move : moves) {
        if (move.isMovedFirst)
            movedFirst.append(move);
    }
    AOT::sort(movedFirst, std::ranges::greater { }, &ForwardedArgument::order);
    for (const ForwardedArgument& move : movedFirst)
        plan.transfers.append(move.transfer);
    for (const ForwardedArgument& load : loads)
        plan.transfers.append(load.transfer);
    for (const ForwardedArgument& move : moves) {
        if (!move.isMovedFirst)
            plan.transfers.append(move.transfer);
    }

    RegisterContents contents;
    for (const RegisterTransfer& transfer : plan.transfers) {
        if (!contents.apply(transfer))
            return std::nullopt;
    }
    for (const RegisterTransfer& transfer : expected) {
        if (contents.of(transfer.destination) != RegisterContents::broughtBy(transfer))
            return std::nullopt;
    }
    return plan;
}

static void emitTransfer(CCallHelpers& jit, const RegisterTransfer& transfer)
{
    if (!transfer.offsetFromSource) {
        jit.move(transfer.source.gpr(), transfer.destination.gpr());
        return;
    }
    CCallHelpers::Address address(transfer.source.gpr(), *transfer.offsetFromSource);
    if (transfer.destination.isFPR())
        jit.loadDouble(address, transfer.destination.fpr());
    else if (transfer.isHalfWord)
        jit.load32(address, transfer.destination.gpr());
    else
        jit.load64(address, transfer.destination.gpr());
}

static CompiledCode generateForwardingCode(const CodeGenerationWorker& worker, const CodeGenerationInputs& inputs, FunctionID id, const ForwardingPlan& plan, CodeGenerationTotals& totals)
{
    BuildContext& build = worker.build;
    const Function& function = inputs.program.at(id);
    CCallHelpers jit;
    RelocationRecorder relocations(jit, build.assemblerPolicy());
    CodeRecords records(inputs.stubCatalog, relocations, totals);
    records.stubCalls.setCaller(id);
    if (!function.inlineFrames.isEmpty())
        records.sites.inlineCallSites.append(0);

    BlockColdness coldness = BlockColdnessRule(inputs, id).of(1, false);
    records.units.willGenerateBlock(coldness.isCold, coldness.isRarelyRun);
    readTagsFromInstance(jit);
    records.entries.append(RecordedEntry { primaryEntry, jit.label() });
    for (const RegisterTransfer& transfer : plan.transfers) {
        CCallHelpers::Label begin = jit.labelIgnoringWatchpoints();
        emitTransfer(jit, transfer);
        records.units.didGenerateInBlock(begin, jit.labelIgnoringWatchpoints(), CodeUnit::Kind::Plain);
    }
    CCallHelpers::Label jump = jit.labelIgnoringWatchpoints();
    records.stubCalls.jumpToFunction(jit, plan.callee, primaryEntry);
    records.units.didGenerateInBlock(jump, jit.labelIgnoringWatchpoints(), CodeUnit::Kind::Pinned);
    CCallHelpers::Label paddingStart = jit.label();
    jit.breakpoint();
    jit.padBeforePatch();

    ImageLinkBuffer buffer(jit, LinkBuffer::Profile::FTL);
    if (buffer.didFailToAllocate()) {
        AOT::reportLinkFailure(LinkFailure::OutOfMemory, id, build);
        return { };
    }
    buffer.finalize(false, "AOT code"_s);
    auto linked = relocations.link(buffer, &build);
    if (!linked) {
        AOT::reportLinkFailure(linked.error(), id, build);
        return { };
    }

    StackCheckState stackChecks;
    return describeCode(build, inputs, id, function, records, EmittedCode { buffer, nullptr, stackChecks, paddingStart, WTF::move(*linked) });
}

static bool isSameCode(const CompiledCode& a, const CompiledCode& b)
{
    auto haveSameEntries = [&] {
        return std::ranges::equal(a.body.entries, b.body.entries, [](const CodeEntry& first, const CodeEntry& second) {
            return first.offset == second.offset && first.id == second.id;
        });
    };
    auto haveSameUnits = [&] {
        return std::ranges::equal(a.body.units.unpack(), b.body.units.unpack(), [](const CodeUnit& first, const CodeUnit& second) {
            return first.offset == second.offset && first.length == second.length && first.kind == second.kind && first.mayRun == second.mayRun;
        });
    };
    return a.body.bytes == b.body.bytes && a.body.paddingStart == b.body.paddingStart && a.body.sizeOfHotBlocks == b.body.sizeOfHotBlocks
        && a.body.relocations == b.body.relocations && a.body.callees == b.body.callees && haveSameEntries() && haveSameUnits()
        && a.body.localSites.size() == b.body.localSites.size() && a.body.returnSites.size() == b.body.returnSites.size()
        && a.frame.hasFrame() == b.frame.hasFrame() && a.frame.prologueChecks.size() == b.frame.prologueChecks.size()
        && a.frame.isEnteredWithoutStackCheck == b.frame.isEnteredWithoutStackCheck && a.frame.isProvenToFitOnStack == b.frame.isProvenToFitOnStack
        && a.sites.positions.size() == b.sites.positions.size() && a.sites.inlineFrames.size() == b.sites.inlineFrames.size();
}

void compileFunctions(const CodeGenerationWorker& worker, const CodeGenerationInputs& inputs, std::span<const FunctionID> ids, std::span<CompiledCode> results, CodeGenerationTotals& totals)
{
    RELEASE_ASSERT(ids.size() == results.size());

    Vector<std::unique_ptr<FunctionCodeGenerator>, 16> generators;
    Vector<B3::Procedure*, 16> procedures;
    const bool comparesWithBackEnd = worker.build.options().validatesBackEnd;
    BitVector isForwardingCode(ids.size());
    CurrentWorkScope workScope(inputs.program, FunctionID());
    currentWork().setPass(PassID::GenerateCode);
    for (size_t index = 0; index < ids.size(); ++index) {
        FunctionID id = ids[index];
        FunctionID idAsRead = findFunctionAsRead(&inputs.program, id);
        currentWork().setFunction(idAsRead);
        if (worker.build.recordedOptions().sabotage) [[unlikely]]
            stopWhereSabotaged(worker.build, inputs.program, inputs.program.at(id), PassID::GenerateCode);
        results[index] = { };
        if (std::optional<ForwardingPlan> plan = planForwarding(worker, inputs, id)) {
            results[index] = generateForwardingCode(worker, inputs, id, *plan, totals);
            isForwardingCode.quickSet(index);
            if (!comparesWithBackEnd) {
                worker.build.statistics().count(StatisticID::FunctionsWithoutFrame);
                worker.build.statistics().count(StatisticID::FunctionsEnteredWithoutStackCheck);
                generators.append(nullptr);
                continue;
            }
        }
        auto generator = FunctionCodeGenerator::create(worker, inputs, id, totals);
        if (generator) {
            generator->procedure().setSubjectOfClient(idAsRead.index() + 1);
            procedures.append(&generator->procedure());
        }
        generators.append(WTF::move(generator));
    }

    B3::prepareForGeneration(procedures.span());
    currentWork().leaveBackEnd();

    for (size_t index = 0; index < ids.size(); ++index) {
        if (generators[index]) {
            currentWork().setFunction(FunctionID(generators[index]->procedure().subjectOfClient() - 1));
            CompiledCode code = generators[index]->finish();
            RELEASE_ASSERT(!isX86_64() || !isForwardingCode.quickGet(index) || isSameCode(results[index], code));
            results[index] = WTF::move(code);
        }
        results[index].function = ids[index];
        generators[index] = nullptr;
    }
}

uint32_t estimatedCodeSize(const Program& program, FunctionID id)
{
    const Function& function = program.at(id);
    uint64_t size = 0;
    for (const Block& block : function.blocks)
        size += estimatedCodeBytes(program, function, block);
    return static_cast<uint32_t>(std::min<uint64_t>(size, UINT32_MAX));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
