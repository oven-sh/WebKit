/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRemoveUnreadExports.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTBuildContext.h"
#include "AOTFlowResults.h"
#include "AOTIR.h"
#include "AOTThreads.h"
#include "AOTValueIdentity.h"
#include <wtf/BitVector.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct FunctionStores {
    FunctionID function;
    Vector<VariableID, 4> variables;
};

} // anonymous namespace

static bool mayBeReadByUnknownKey(NamespaceUse use)
{
    switch (use) {
    case NamespaceUse::Unknown:
        return true;
    case NamespaceUse::NamedReadsOnly:
    case NamespaceUse::NeverCreated:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Whether a static field exists decides where a store by name to its class or to a subclass goes, so its definition is no dead store.
static bool isStaticField(const Variable& variable)
{
    return !!variable.owner;
}

static BitVector findExportedVariables(const Program& program)
{
    BitVector exportedVariables(program.variables.size());
    auto add = [&](VariableID variable) {
        if (variable)
            exportedVariables.quickSet(variable.index());
    };
    for (const Module& module : program.modules) {
        for (const ModuleExport& entry : module.exports)
            add(entry.variable);
        for (const SyntheticExport& entry : module.syntheticExports)
            add(entry.variable);
    }
    return exportedVariables;
}

static BitVector findUnreadVariables(const Program& program, const FlowResults& flow)
{
    BitVector exportedVariables = findExportedVariables(program);
    BitVector unreadVariables;
    for (unsigned index = 0; index < program.variables.size(); ++index) {
        const Variable& variable = program.variables[index];
        bool mayBeReadThroughNamespace = exportedVariables.quickGet(index) && mayBeReadByUnknownKey(flow.module(variable.module).namespaceUse);
        if (variable.flow.reads == ReadState::NeverRead
            && variable.flow.writers == WriterState::AllKnown
            && !isStaticField(variable)
            && !mayBeReadThroughNamespace)
            unreadVariables.set(index);
    }
    return unreadVariables;
}

static bool isStoreTo(const Value& value, const BitVector& variables)
{
    return value.opcode == Opcode::PutModuleVariable && variables.get(value.variable().index());
}

static Vector<FunctionStores> findStores(const Program& program, const BitVector& variables, const ParallelLoops& loops)
{
    return collectInParallel<FunctionStores>(loops, program.functions.size(), [&](unsigned index, Vector<FunctionStores>& found) {
        Vector<VariableID, 4> storedVariables;
        forEachListedValue(program, FunctionID(index), { Opcode::PutModuleVariable }, [&](BlockID, uint32_t, ValueID, const Value& value) {
            if (isStoreTo(value, variables))
                storedVariables.append(value.variable());
        });
        if (!storedVariables.isEmpty())
            found.append({ FunctionID(index), WTF::move(storedVariables) });
    });
}

static void removeStores(Function& function, const BitVector& variables)
{
    for (Block& block : function.editAllBlocks()) {
        block.values.removeAllMatching([&](ValueID id) {
            return isStoreTo(function.at(id), variables);
        });
    }
}

static bool isAnnotatedPure(const Value& value)
{
    return (value.opcode == Opcode::Call || value.opcode == Opcode::Construct) && value.isAnnotatedPure();
}

// The annotation says that the call may go when nobody uses its answer. An argument can be such a call too, and a cast that tests nothing is no use by itself.
static void removeUnusedPureCalls(Function& function)
{
    bool didRemove = true;
    while (didRemove) {
        BitVector usedValues(function.values.size());
        auto markUsed = [&](ValueID value) {
            usedValues.quickSet(value.index());
        };
        for (const Block& block : function.blocks) {
            for (ValueID id : block.values)
                function.at(id).forEachChild(markUsed);
            block.forEachChildOutsideValues(markUsed);
        }

        didRemove = false;
        for (Block& block : function.editAllBlocks()) {
            didRemove |= !!block.values.removeAllMatching([&](ValueID id) {
                const Value& value = function.at(id);
                return (isAnnotatedPure(value) || isCastWithoutTest(value.opcode)) && !usedValues.quickGet(id.index());
            });
        }
    }
}

void removeUnreadExports(ProgramEditor& editor, const FlowResults& flow, BuildContext& build)
{
    if (!flow.hasResults())
        return;

    const Program& program = editor.program();
    BitVector unreadVariables = findUnreadVariables(program, flow);
    if (unreadVariables.isEmpty())
        return;

    const ParallelLoops& loops = build.parallelLoops();
    Vector<Function*> editedFunctions;
    BitVector variablesWithRemovedStores;
    BitVector variablesWithRemainingStores;
    uint64_t numberOfRemovedStores = 0;
    for (const FunctionStores& stores : findStores(program, unreadVariables, loops)) {
        bool isEdited = build.isEnabled(PassID::RemoveUnreadExports, stores.function);
        BitVector& storedVariables = isEdited ? variablesWithRemovedStores : variablesWithRemainingStores;
        for (VariableID variable : stores.variables)
            storedVariables.set(variable.index());
        if (!isEdited)
            continue;
        editedFunctions.append(&editor.at(stores.function));
        numberOfRemovedStores += stores.variables.size();
    }

    forEachIndexInParallel(loops, editedFunctions.size(), [&](unsigned index) {
        removeStores(*editedFunctions[index], unreadVariables);
        removeUnusedPureCalls(*editedFunctions[index]);
    });

    variablesWithRemovedStores.exclude(variablesWithRemainingStores);
    for (size_t index : variablesWithRemovedStores)
        editor.at(VariableID(index)).flow.stores = StoreCount::Never;
    build.statistics().count(StatisticID::UnreadVariableStoresRemoved, numberOfRemovedStores);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
