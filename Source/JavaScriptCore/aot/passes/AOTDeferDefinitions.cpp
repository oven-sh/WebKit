/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTDeferDefinitions.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIR.h"
#include "AOTKnownClosures.h"
#include "AOTThreads.h"
#include "AOTVerifier.h"
#include <algorithm>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct Mention {
    enum class Kind : uint8_t { Store, Closure };

    Kind kind { Kind::Store };
    uint32_t subject { 0 };
};

struct DeferredStore {
    FunctionID body;
    ValueID store;
    ValueID closure;
    VariableID variable;
    FunctionID function;
};

} // anonymous namespace

static Vector<uint8_t> countUpToTwo(std::span<const Mention> mentions, Mention::Kind kind, size_t numberOfSubjects)
{
    Vector<uint8_t> counts(FillWith { }, numberOfSubjects, static_cast<uint8_t>(0));
    for (const Mention& mention : mentions) {
        if (mention.kind == kind)
            counts[mention.subject] = std::min<unsigned>(counts[mention.subject] + 1, 2);
    }
    return counts;
}

// The closure table has one cell per function, so the variable has to hold the only closure that the function ever has.
static bool canBeBornAsFunction(const Program& program, const Variable& variable)
{
    if (variable.initialization == VariableInitialization::Function || variable.owner)
        return false;
    if (!variable.module || !hoistsFunctionsIntoVariables(program.at(variable.module).kind))
        return false;
    return variable.flow.writers == WriterState::AllKnown && variable.flow.stores == StoreCount::AtMostOnce && variable.flow.initialization != InitializationState::Unknown;
}

static void findDeferredStores(const Program& program, BuildContext& build, FunctionID id, std::span<const uint8_t> storesOfVariable, std::span<const uint8_t> closuresOfFunction, Vector<DeferredStore>& found)
{
    const Function& function = program.at(id);
    std::optional<FunctionAnalyses> analyses;
    for (const Block& block : function.blocks) {
        for (ValueID storeID : block.values) {
            const Value& store = function.at(storeID);
            if (store.opcode != Opcode::PutModuleVariable || storesOfVariable[store.variable().index()] != 1 || !canBeBornAsFunction(program, program.at(store.variable())))
                continue;
            ValueID closureID = store.valueOperand();
            const Value& closure = function.at(closureID);
            if (closure.opcode != Opcode::NewClosure || !closure.variadicOperands().empty() || closuresOfFunction[closure.function().index()] != 1)
                continue;
            if (!analyses)
                analyses.emplace(AnalysisContext { build, program, nullptr, nullptr, id }, function);
            if (analyses->useLists().useCount(closureID) == 1)
                found.append({ id, storeID, closureID, store.variable(), closure.function() });
        }
    }
}

void deferDefinitions(ProgramEditor& editor, BuildContext& build)
{
    const Program& program = editor.program();
    const ParallelLoops& loops = build.parallelLoops();
    Vector<Mention> mentions = collectInParallel<Mention>(loops, program.functions.size(), [&](unsigned index, Vector<Mention>& found) {
        const Function& function = program.functions[index];
        for (const Block& block : function.blocks) {
            for (ValueID id : block.values) {
                const Value& value = function.at(id);
                if (value.opcode == Opcode::PutModuleVariable)
                    found.append({ Mention::Kind::Store, value.variable().index() });
                else if (value.opcode == Opcode::NewClosure)
                    found.append({ Mention::Kind::Closure, value.function().index() });
            }
        }
    });
    Vector<uint8_t> storesOfVariable = countUpToTwo(mentions.span(), Mention::Kind::Store, program.variables.size());
    Vector<uint8_t> closuresOfFunction = countUpToTwo(mentions.span(), Mention::Kind::Closure, program.functions.size());
    for (const Variable& variable : program.variables) {
        if (variable.initialization == VariableInitialization::Function && variable.function)
            closuresOfFunction[variable.function.index()] = 2;
    }

    Vector<DeferredStore> stores = collectInParallel<DeferredStore>(loops, program.functions.size(), [&](unsigned index, Vector<DeferredStore>& found) {
        if (program.functions[index].hasBody() && build.isEnabled(PassID::DeferDefinitions, FunctionID(index)))
            findDeferredStores(program, build, FunctionID(index), storesOfVariable.span(), closuresOfFunction.span(), found);
    });

    for (const DeferredStore& deferred : stores) {
        for (Block& block : editor.at(deferred.body).blocks) {
            block.values.removeAllMatching([&](ValueID id) {
                return id == deferred.store || id == deferred.closure;
            });
        }
        Variable& variable = editor.at(deferred.variable);
        variable.initialization = VariableInitialization::Function;
        variable.function = deferred.function;
        variable.flow.stores = StoreCount::Never;
    }
    build.statistics().count(StatisticID::ClosuresDeferredInVariables, stores.size());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
