/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowApply.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTFlowSiteData.h"
#include "AOTFunctionEditing.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include <span>

namespace JSC { namespace AOT { namespace Typed {

static void applyEntryParameterUpdates(Function& function, std::span<const FlowPositionUpdate> updates)
{
    if (updates.empty())
        return;
    RELEASE_ASSERT(updates.size() == function.numberOfEntryParameters());
    for (unsigned index = 0; index < updates.size(); ++index) {
        if (DeclaredType* declared = function.declaredEntryParameter(index))
            applyPosition(*declared, updates[index]);
    }
    // What a value was expected to be is met with what it is expected to be now, and nothing but the positions has spoken so far.
    if (function.hasBody() && std::ranges::any_of(updates, &FlowPositionUpdate::replacesExpectation)) {
        for (Value& value : function.editAllValues())
            value.setExpected({ });
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

void applySignatureUpdates(const FlowApplyContext& context)
{
    const Vector<FlowSignatureUpdate>& updates = context.application.signatures;
    RELEASE_ASSERT(updates.size() == context.application.functionCountAsRead);

    forEachIndexInParallel(context.build.parallelLoops(), updates.size(), [&](unsigned index) {
        Function& function = context.program.functions[index];
        const FlowSignatureUpdate& update = updates[index];
        applyEntryParameterUpdates(function, update.entryParameters.span());
        applyResultUpdates(function, update.results.span());
        function.capabilities.add(update.provenCapabilities);
    });
}

void removeUnreachedBodies(const FlowApplyContext& context, ProgramEditor& editor)
{
    forEachIndexInParallel(context.build.parallelLoops(), context.results.functionCountAsRead(), [&](unsigned index) {
        FunctionID id(index);
        if (context.results.function(id).reachability == FunctionReachability::Unreached && context.program.at(id).hasBody())
            editor.removeBody(id);
    });
}

// A record of constant data that only unreached code names is never made. The types of fields are narrowed to what reached code stores, so such a record may no longer fit the fields that it fills.
void dropDataOfUnreachedCode(const FlowApplyContext& context)
{
    Program& program = context.program;
    Vector<DataID> named = collectInParallel<DataID>(context.build.parallelLoops(), program.functions.size(), [&](unsigned index, Vector<DataID>& found) {
        for (const Value& value : program.functions[index].values) {
            if (Value::opcodesWithData.contains(value.opcode))
                found.append(value.data());
        }
    });
    BitVector isNamed(program.data.size());
    for (DataID data : named)
        isNamed.quickSet(data.index());
    // The host makes what a module of data exports.
    for (const Module& module : program.modules) {
        if (module.data)
            isNamed.quickSet(module.data.index());
    }
    for (uint32_t index = 0; index < program.data.size(); ++index) {
        if (!isNamed.quickGet(index))
            program.data[index].root = DataNode();
    }
}

// The readers of an array go by what flow says it holds. An array of a record that stays constant data begins with the storage that fits that too, as a literal that ExpandData makes of a small record does.
void narrowContentsOfData(const FlowApplyContext& context)
{
    struct RootSite {
        DataID data;
        AllocationSiteID site;
    };

    Program& program = context.program;
    const ParallelLoops& loops = context.build.parallelLoops();
    Vector<RootSite> sites = collectInParallel<RootSite>(loops, context.application.bodies.size(), [&](unsigned index, Vector<RootSite>& found) {
        const Function& function = program.functions[index];
        if (!function.hasBody())
            return;
        for (const FlowAllocationStamp& stamp : context.application.bodies[index].allocations) {
            const Value& value = function.at(stamp.value);
            if (value.opcode == Opcode::Data)
                found.append({ value.data(), stamp.site });
        }
    });
    Vector<AllocationSiteID> rootSites(program.data.size());
    BitVector isNamedTwice(program.data.size());
    for (const RootSite& root : sites) {
        AllocationSiteID& rootSite = rootSites[root.data.index()];
        if (rootSite)
            isNamedTwice.quickSet(root.data.index());
        rootSite = root.site;
    }
    forEachIndexInParallel(loops, program.data.size(), [&](unsigned index) {
        if (!rootSites[index] || isNamedTwice.quickGet(index))
            return;
        forEachDataNodeInPreorder(program.data[index].root, [&](uint32_t ordinal, const DataNode& node) {
            const auto* array = node.tryAs<DataNode::Array>();
            if (!array || !isStorageOfArray(*array))
                return;
            if (TypeID elements = context.results.dataNodeStamp(rootSites[index], ordinal).elements)
                const_cast<DataNode::Array*>(array)->contents = elements;
        });
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
