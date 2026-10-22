/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCreateEntrypointsInternal.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTDerivedFunctions.h"
#include "AOTFlowResults.h"
#include "AOTInterningPhases.h"
#include "AOTSlotPlan.h"
#include "AOTThreads.h"
#include "AOTTypeArena.h"
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

Function createDerivedHeader(const Function& target, FunctionID targetID, FunctionRole role, uint32_t keyParameter)
{
    Function header;
    header.name = target.name;
    header.module = target.module;
    header.position = target.position;
    header.text = target.text;
    header.kind = target.kind;
    header.role = role;
    header.flags = target.flags & FunctionFlags::IsChecked;
    header.length = target.length;
    header.enclosingRuntimeFunction = target.enclosingRuntimeFunction;
    header.thisType = DeclaredType { .type = TypeTable::anything };
    header.capabilities = capabilitiesOf(role);
    header.derivation = Derivation { .source = targetID, .parameter = keyParameter };
    return header;
}

void emitReturnOfEntrypointCall(IRBuilder& builder, ValueID call)
{
    const Function& function = builder.function();
    const Block& block = function.at(builder.currentBlock());
    RELEASE_ASSERT(!block.handler && block.values.last() == call && function.at(call).opcode == Opcode::CallDirect);
    if (builder.context().program.typeOf(function, call).isNever()) {
        builder.emitUnreachable();
        return;
    }

    // The frame stays only where the callee returns its result in another form than the entrypoint.
    // A callee on a cycle of tail calls that leads back through the entrypoint returns it in the same form.
    Terminator terminator = IRBuilder::makeTerminator(TerminatorKind::ReturnCall, { call });
    terminator.setRequirement(TailCallRequirement::Allowed);
    builder.emitTerminator(WTF::move(terminator));
}

static bool mayBeReadWithItsFunction(const Variable& variable)
{
    switch (variable.flow.reads) {
    case ReadState::Unknown:
    case ReadState::OnlyAccessed:
        return true;
    case ReadState::OnlyCalled:
        // A call cannot name its callee while another value may be stored, so it reads the variable.
        return variable.flow.stores != StoreCount::Never;
    case ReadState::NeverRead:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The functions whose closure exists without a NewClosure: a read of the variable that starts with the function creates it.
static BitVector findFunctionsReadFromVariables(const Program& program, const FlowResults& flow)
{
    BitVector described = findVariablesDescribedToRunTime(program, flow);
    BitVector functions(program.functions.size());
    for (unsigned index = 0; index < program.variables.size(); ++index) {
        const Variable& variable = program.variables[index];
        if (variable.function && (described.quickGet(index) || mayBeReadWithItsFunction(variable)))
            functions.quickSet(variable.function.index());
    }
    return functions;
}

static BitVector findFunctionsEnteredWithoutReference(const Program& program, const FlowResults& flow, const ParallelLoops& loops)
{
    BitVector calledByHost = functionsCalledByHost(program, loops);
    BitVector readFromVariables = findFunctionsReadFromVariables(program, flow);

    BitVector entered(program.functions.size());
    auto found = findIndicesInParallel(loops, program.functions.size(), [&](unsigned index) {
        const Function& function = program.functions[index];
        // A function that is read and never called loses its body. The read still creates the closure, whose identity is the entrypoint.
        bool isEntered = readFromVariables.quickGet(index) || (calledByHost.quickGet(index) && function.hasBody());
        return isEntered && canBeCalledByValueOf(function.role);
    });
    for (uint32_t index : found)
        entered.quickSet(index);
    return entered;
}

static FunctionID addConstructEntrypoint(const Program& program, const GenericEntrypointRequest& request, NOESCAPE const Invocable<FunctionID(const GenericEntrypointRequest&)> auto& add)
{
    if (entrypointVariantsOf(program, program.at(request.target)) != EntrypointVariants::Both)
        return { };
    GenericEntrypointRequest constructRequest = request;
    constructRequest.takesNewTarget = true;
    constructRequest.allocatesThis = hasAllocatingConstructEntrypoint(program, program.at(request.target));
    return add(constructRequest);
}

static FunctionID addEntrypointNamedByClosures(const Program& program, GenericEntrypointRequest request, NOESCAPE const Invocable<FunctionID(const GenericEntrypointRequest&)> auto& add)
{
    request.takesNewTarget = entrypointVariantsOf(program, program.at(request.target)) == EntrypointVariants::WithNewTarget;
    return add(request);
}

static FunctionID addGenericEntrypoints(const Program& program, GenericEntrypointRequest request, NOESCAPE const Invocable<FunctionID(const GenericEntrypointRequest&)> auto& add)
{
    request.constructEntrypoint = addConstructEntrypoint(program, request, add);
    return addEntrypointNamedByClosures(program, request, add);
}

// The entrypoints get the ids, and their types the ids, that they would get one after the other.
static void addEntrypointsForHost(ProgramEditor& editor, const AnalysisContext& context, std::span<const FunctionID> targets)
{
    if (targets.empty())
        return;

    const Program& program = editor.program();
    const ParallelLoops& loops = context.build.parallelLoops();
    unsigned count = static_cast<unsigned>(targets.size());
    Vector<uint32_t> firstAdded;
    firstAdded.reserveInitialCapacity(count);
    unsigned numberAdded = 0;
    for (FunctionID target : targets) {
        firstAdded.append(numberAdded);
        numberAdded += entrypointVariantsOf(program, program.at(target)) == EntrypointVariants::Both ? 2 : 1;
    }
    uint32_t firstID = editor.reserveFunctions(numberAdded).index();

    InterningPhases phases(editor, count);
    forEachIndexInParallel(loops, count, [&](unsigned position) {
        TaskArenas arenas(phases, position);
        uint32_t nextID = firstID + firstAdded[position];
        addGenericEntrypoints(program, { .target = targets[position], .isEnteredByHost = true }, [&](const GenericEntrypointRequest& variant) {
            FunctionID id(nextID++);
            editor.replaceFunctionLike(id, variant.target, createGenericEntrypoint(context, arenas.types, arenas.strings, variant));
            return id;
        });
    });

    BodyRemaps remaps = phases.finish();
    if (remaps.isIdentity())
        return;
    forEachIndexInParallel(loops, numberAdded, [&](unsigned index) {
        applyRemaps(remaps, editor.at(FunctionID(firstID + index)));
    });
}

void createEntrypoints(ProgramEditor& editor, const OptimizationInputs& inputs, BuildContext& build)
{
    const Program& program = editor.program();
    AnalysisContext context { build, program, &inputs.flow, nullptr };
    size_t numberOfFunctionsBefore = program.functions.size();

    Vector<FunctionID> entered;
    for (size_t index : findFunctionsEnteredWithoutReference(program, inputs.flow, build.parallelLoops()))
        entered.append(FunctionID(index));
    addEntrypointsForHost(editor, context, entered.span());

    uint64_t numberOfAddedFunctions = program.functions.size() - numberOfFunctionsBefore;
    build.statistics().count(StatisticID::GenericEntrypoints, numberOfAddedFunctions);
    build.statistics().countChanges(PassID::CreateEntrypoints, numberOfAddedFunctions);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
