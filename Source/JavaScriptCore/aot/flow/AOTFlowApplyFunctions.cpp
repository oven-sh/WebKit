/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowApply.h"

#if ENABLE(AOT)

#include "AOTCreateBoundFunctions.h"
#include "AOTFunctionEditing.h"
#include "AOTSortedSet.h"
#include "AOTThreads.h"
#include <span>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

static Function createVersion(const Program& program, FunctionID source, FunctionRole role)
{
    const Function& original = program.at(source);
    Function copy = original;
    copy.role = role;
    copy.creator = CreatorRecord::Version { source };
    copy.derivation = Derivation {
        .source = source,
        .parameterMap = Vector<ParameterOrigin>(original.numberOfEntryParameters(), [](size_t index) {
            return ParameterOrigin { static_cast<uint32_t>(index) };
        }),
    };
    copy.capabilities = original.capabilities & capabilitiesOf(role);
    if (role == FunctionRole::CallSiteVersion)
        copy.capabilities.add(FunctionCapabilities::HasOnlyDirectCallers);
    return copy;
}

void createVersions(const FlowApplyContext& context, ProgramEditor& editor)
{
    const FlowVersionPlan& plan = context.application.versions;
    unsigned count = plan.functionCount() - plan.functionCountAsRead;
    if (!count)
        return;

    FunctionID first = editor.reserveFunctions(count);
    RELEASE_ASSERT(first.index() == plan.functionCountAsRead);
    Vector<Function> copies(count);
    forEachIndexInParallel(context.build.parallelLoops(), count, [&](unsigned index) {
        FunctionID version(first.index() + index);
        copies[index] = createVersion(context.program, plan.bodyOf(version), plan.isGenericVersion(version) ? FunctionRole::GenericVersion : FunctionRole::CallSiteVersion);
    });
    for (unsigned index = 0; index < count; ++index)
        editor.replaceFunction(FunctionID(first.index() + index), WTF::move(copies[index]));
}

void createBoundFunctions(const FlowApplyContext& context, ProgramEditor& editor)
{
    const FlowBoundFunctionPlan& plan = context.application.boundFunctions;
    unsigned count = plan.targets.size();
    if (!count)
        return;

    FunctionID first = editor.reserveFunctions(count);
    RELEASE_ASSERT(first.index() == plan.firstFunction);
    Vector<FunctionID> functions(count, [&](size_t index) {
        return plan.functionAt(index);
    });
    editFunctionsInParallel(editor, context.build, functions.span(), [&](unsigned index, FunctionID, Function& function, TaskArenas& arenas) {
        function = createBoundFunction(context.program, context.results, arenas.types, context.build, plan.targets[index], 0);
    });
    context.build.statistics().count(StatisticID::BoundFunctions, count);
}

static void applyEntryParameterUpdates(Function& function, std::span<const FlowPositionUpdate> updates)
{
    if (updates.empty())
        return;
    RELEASE_ASSERT(updates.size() == function.numberOfEntryParameters());
    for (unsigned index = 0; index < updates.size(); ++index) {
        if (DeclaredType* declared = function.declaredEntryParameter(index))
            applyPosition(*declared, updates[index]);
    }
}

static void applyResultUpdates(Function& function, std::span<const FlowPositionUpdate> updates)
{
    if (updates.empty())
        return;
    RELEASE_ASSERT(updates.size() == function.results.size());
    for (size_t index = 0; index < updates.size(); ++index)
        applyPosition(function.results[index], updates[index]);
}

// A generic version takes anything. What the calls that flow follows pass is what the other calls are expected to pass.
static void predictEntryParameters(Function& genericVersion, const FlowSignatureUpdate& updateOfProvenVersion)
{
    std::span<const FlowPositionUpdate> updates = updateOfProvenVersion.entryParameters.span();
    std::span<const TypeID> predictions = updateOfProvenVersion.predictionsOfGenericVersion.span();
    RELEASE_ASSERT(updates.empty() || updates.size() == genericVersion.numberOfEntryParameters());
    RELEASE_ASSERT(predictions.empty() || predictions.size() == genericVersion.numberOfEntryParameters());
    for (unsigned index = 0; index < genericVersion.numberOfEntryParameters(); ++index) {
        DeclaredType* declared = genericVersion.declaredEntryParameter(index);
        if (!declared || declared->predicted)
            continue;
        if (!predictions.empty() && predictions[index])
            declared->predicted = predictions[index];
        else if (!updates.empty())
            declared->predicted = updates[index].predicted ? updates[index].predicted : updates[index].type;
    }
}

// A version declares the parameters that the function declares as read, so every call fits them. Its result is whatever flows in once its callers test it.
static void leaveResultTestToCallers(Function& version)
{
    for (DeclaredType& result : version.results) {
        if (!result.written)
            result.written = result.type;
        result.type = TypeTable::anything;
    }
}

void applySignatureUpdates(const FlowApplyContext& context)
{
    const Vector<FlowSignatureUpdate>& updates = context.application.signatures;
    const FlowVersionPlan& versions = context.application.versions;
    RELEASE_ASSERT(updates.size() == versions.functionCountAsRead);

    forEachIndexInParallel(context.build.parallelLoops(), updates.size(), [&](unsigned index) {
        Function& function = context.program.functions[index];
        const FlowSignatureUpdate& update = updates[index];
        applyEntryParameterUpdates(function, update.entryParameters.span());
        applyResultUpdates(function, update.results.span());
        function.capabilities.add(update.provenCapabilities);
    });
    forEachIndexInParallel(context.build.parallelLoops(), versions.provenVersions.size(), [&](unsigned index) {
        FunctionID provenVersion = versions.provenVersions[index];
        predictEntryParameters(context.program.at(versions.genericVersionOf(provenVersion)), updates[provenVersion.index()]);
    });
    forEachIndexInParallel(context.build.parallelLoops(), versions.functionCount() - versions.functionCountAsRead, [&](unsigned index) {
        FunctionID version(versions.functionCountAsRead + index);
        if (context.results.function(version).resultCheck == ResultCheck::ByCallers)
            leaveResultTestToCallers(context.program.at(version));
    });
}

// A member of a class is expected to be called on an instance of the class or of a class below it. Where the function has escaped, flow says nothing about the receivers
// that arrive, because any may. What flow does say comes first.
void predictReceiversOfMembers(const FlowApplyContext& context, ProgramEditor& editor)
{
    Program& program = context.program;
    const FlowVersionPlan& versions = context.application.versions;
    // A prediction of more layouts than a site tests for buys nothing.
    size_t maxReceivers = static_cast<size_t>(context.build.limit(LimitID::MaxDispatchCases));
    Vector<SortedSet<LayoutID, 2>> receiversOfMember(versions.functionCountAsRead);
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        const Layout& instances = program.layouts[index];
        if (instances.kind != LayoutKind::Instance || !instances.constructor || instances.flow.allocation == AllocationState::NeverAllocated)
            continue;
        // Predicted bases may form a cycle.
        Vector<LayoutID, 8> visitedClasses;
        for (LayoutID current(index); current && !visitedClasses.contains(current); current = expectedBaseOf(program.at(current))) {
            visitedClasses.append(current);
            for (const ClassMember& member : program.at(current).classFlow.members) {
                SortedSet<LayoutID, 2>& receivers = receiversOfMember[member.function.index()];
                if (member.side == ClassSide::Prototype && receivers.size() <= maxReceivers)
                    receivers.add(LayoutID(index));
            }
        }
    }

    TypeArena types(editor.types());
    for (uint32_t index = 0; index < versions.functionCount(); ++index) {
        Function& function = program.functions[index];
        const SortedSet<LayoutID, 2>& receivers = receiversOfMember[versions.bodyOf(FunctionID(index)).index()];
        if (receivers.isEmpty() || receivers.size() > maxReceivers || !function.has(FunctionFlags::HasThis) || function.thisType.predicted)
            continue;
        TypeID predicted = types.intersectionOf(function.thisType.type, types.intern(Type::forLayouts(receivers.span(), types)));
        if (predicted != TypeTable::never && predicted != function.thisType.type)
            function.thisType.predicted = predicted;
    }
}

void removeUnreachedBodies(const FlowApplyContext& context, ProgramEditor& editor)
{
    forEachIndexInParallel(context.build.parallelLoops(), context.results.functionCountAsRead(), [&](unsigned index) {
        FunctionID id(index);
        if (context.results.function(id).reachability == FunctionReachability::Unreached && context.program.at(id).hasBody())
            editor.removeBody(id);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
