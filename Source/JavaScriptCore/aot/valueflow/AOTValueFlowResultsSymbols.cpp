/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowResultsInternal.h"

#if ENABLE(AOT)

#include "AOTBlockCycles.h"
#include "AOTNameCreatedSymbols.h"
#include "AOTThreads.h"
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

namespace {

// A call that the rule for Symbol answers with the key, should the call enter Symbol.
struct CallNamedByKey {
    PropertyKeyID key;
    // It enters Symbol and nothing else, and it runs at most once.
    bool createsOneSymbol { false };
};

struct ClosureMade {
    FunctionID function;
    bool isMadeAtMostOnce { false };
};

} // anonymous namespace

static BitVector findModulesWithCreatedSymbols(const Program& program)
{
    BitVector modules(program.modules.size());
    for (uint32_t index = 0, count = program.types.numberOfPropertyKeys(); index < count; ++index) {
        const PropertyKey& key = program.at(PropertyKeyID(index));
        if (key.kind == PropertyKeyKind::CreatedSymbol)
            modules.set(key.module.index());
    }
    return modules;
}

// The constant takes the place of the call, and the literal goes into the types of whatever holds the symbol.
static bool mayReplaceCallsIn(const ValueFlowResultInputs& inputs, FunctionID function)
{
    return inputs.build.isEnabled(FlowResultKind::LiteralValues, function.index()) && inputs.build.isEnabled(FlowResultKind::NarrowedTypes, function.index());
}

static void appendCallsNamedByKeys(const ValueFlowResultInputs& inputs, const FlowResultTables& tables, const BitVector& modulesWithCreatedSymbols, FunctionID id, Vector<CallNamedByKey>& calls)
{
    const Program& program = inputs.program;
    const Function& function = program.at(id);
    if (!function.module || !modulesWithCreatedSymbols.get(function.module.index()))
        return;
    // No other function has code.
    if (!inputs.isReached(id) && !isUnsolvedCode(inputs, tables, id))
        return;

    // Only where the solve follows the values has the rule answered the call, and only there does the constant replace it. Anywhere else the call stays and makes a symbol of its own.
    bool runsAtMostOnce = inputs.isReached(id)
        && tables.functions.functions[id.index()].executions == ExecutionCount::AtMostOnce && mayReplaceCallsIn(inputs, id);

    std::optional<CFG> cfg;
    std::optional<BlockCycles> cycles;
    for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        for (ValueID value : function.blocks[blockIndex].values) {
            const Value& operation = function.at(value);
            std::optional<PropertyKey> named = keyOfSymbolsCreatedBy(function, operation);
            std::optional<PropertyKeyID> key = named ? program.types.find(*named) : std::nullopt;
            if (!key)
                continue;

            bool createsOneSymbol = runsAtMostOnce && callsSymbolDirectly(program.builtins(), function, operation);
            if (createsOneSymbol) {
                if (!cycles) {
                    cfg.emplace(function);
                    cycles.emplace(*cfg);
                }
                createsOneSymbol = !cycles->isInCycle(BlockID(blockIndex));
            }
            calls.append(CallNamedByKey { *key, createsOneSymbol });
        }
    }
}

// In the solve a key stands for every symbol that the calls at its position make. It is one symbol, and so a constant, where one call is all there is and that call runs at most once.
BitVector computeCreatedSymbolResults(const ValueFlowResultInputs& inputs, const FlowResultTables& tables)
{
    const Program& program = inputs.program;
    unsigned numberOfKeys = program.types.numberOfPropertyKeys();
    BitVector symbolsCreatedOnce(numberOfKeys);
    BitVector modulesWithCreatedSymbols = findModulesWithCreatedSymbols(program);
    if (modulesWithCreatedSymbols.isEmpty())
        return symbolsCreatedOnce;

    Vector<CallNamedByKey> calls = collectInParallel<CallNamedByKey>(inputs.loops(), program.functions.size(), [&](unsigned index, Vector<CallNamedByKey>& found) {
        appendCallsNamedByKeys(inputs, tables, modulesWithCreatedSymbols, FunctionID(index), found);
    });

    // The copies of an operation keep its position: the two versions of a typed function, the two ways through a finally block. Each copy makes a symbol of its own.
    BitVector keysOfCalls(numberOfKeys);
    for (const CallNamedByKey& call : calls) {
        bool isNamedByOtherCall = keysOfCalls.set(call.key.index());
        symbolsCreatedOnce.set(call.key.index(), call.createsOneSymbol && !isNamedByOtherCall);
    }
    return symbolsCreatedOnce;
}

static void appendClosuresMadeBy(const ValueFlowResultInputs& inputs, const FlowResultTables& tables, FunctionID id, Vector<ClosureMade>& closures)
{
    const Function& function = inputs.program.at(id);
    // No other function has code.
    if (!function.hasBody() || (!inputs.isReached(id) && !isUnsolvedCode(inputs, tables, id)))
        return;

    bool runsAtMostOnce = inputs.isReached(id) && tables.functions.functions[id.index()].executions == ExecutionCount::AtMostOnce;
    std::optional<CFG> cfg;
    std::optional<BlockCycles> cycles;
    for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        for (ValueID value : function.blocks[blockIndex].values) {
            const Value& operation = function.at(value);
            if (operation.opcode != Opcode::NewClosure)
                continue;
            if (runsAtMostOnce && !cycles) {
                cfg.emplace(function);
                cycles.emplace(*cfg);
            }
            closures.append(ClosureMade { operation.function(), runsAtMostOnce && !cycles->isInCycle(BlockID(blockIndex)) });
        }
    }
}

// `const emit = function () { .. }` in the code of a module. What a class definition, a variable that declares a function or the run time makes is not counted here: such a function is not listed.
void computeClosuresMadeOnce(const ValueFlowResultInputs& inputs, FlowResultTables& tables)
{
    const Program& program = inputs.program;
    Vector<ClosureMade> closures = collectInParallel<ClosureMade>(inputs.loops(), program.functions.size(), [&](unsigned index, Vector<ClosureMade>& found) {
        appendClosuresMadeBy(inputs, tables, FunctionID(index), found);
    });

    BitVector isMadeSomewhere(program.functions.size());
    BitVector& hasOneClosure = tables.functions.functionsWithOneClosure;
    hasOneClosure = BitVector(program.functions.size());
    for (const ClosureMade& closure : closures) {
        bool isMadeElsewhere = isMadeSomewhere.set(closure.function.index());
        hasOneClosure.set(closure.function.index(), closure.isMadeAtMostOnce && !isMadeElsewhere);
    }
    for (const Variable& variable : program.variables) {
        if (variable.initialization == VariableInitialization::Function && variable.function)
            hasOneClosure.clear(variable.function.index());
    }
    for (const Layout& layout : program.layouts) {
        for (const ClassMember& member : layout.classFlow.members) {
            if (member.function && !layout.has(LayoutFlags::IsConstructorFunction))
                hasOneClosure.clear(member.function.index());
        }
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
