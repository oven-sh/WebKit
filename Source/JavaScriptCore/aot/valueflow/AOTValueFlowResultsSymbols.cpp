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

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct CyclesOnDemand {
    const Function& function;
    std::optional<CFG> cfg { };
    std::optional<BlockCycles> cycles { };

    bool isInCycle(uint32_t block)
    {
        if (!cycles) {
            cfg.emplace(function);
            cycles.emplace(*cfg);
        }
        return cycles->isInCycle(BlockID(block));
    }
};

} // anonymous namespace

BitVector computeCreatedSymbolResults(const ValueFlowResultInputs& inputs, const FlowResultTables& tables)
{
    const Program& program = inputs.program;
    unsigned numberOfKeys = program.types.numberOfPropertyKeys();
    BitVector symbolsCreatedOnce(numberOfKeys);
    BitVector modulesWithCreatedSymbols(program.modules.size());
    for (uint32_t index = 0; index < numberOfKeys; ++index) {
        const PropertyKey& key = program.at(PropertyKeyID(index));
        if (key.kind == PropertyKeyKind::CreatedSymbol)
            modulesWithCreatedSymbols.set(key.module.index());
    }
    if (modulesWithCreatedSymbols.isEmpty())
        return symbolsCreatedOnce;

    struct CallNamedByKey {
        PropertyKeyID key;
        bool createsOneSymbol { false };
    };
    Vector<CallNamedByKey> calls = collectInParallel<CallNamedByKey>(inputs.loops(), program.functions.size(), [&](unsigned index, Vector<CallNamedByKey>& found) {
        FunctionID id(index);
        const Function& function = program.at(id);
        if (!function.module || !modulesWithCreatedSymbols.get(function.module.index()))
            return;
        if (!inputs.isReached(id) && !isUnsolvedCode(inputs, tables, id))
            return;

        bool runsAtMostOnce = inputs.isReached(id) && tables.functions.functions[index].executions == ExecutionCount::AtMostOnce
            && inputs.build.isEnabled(FlowResultKind::LiteralValues, index) && inputs.build.isEnabled(FlowResultKind::NarrowedTypes, index);

        CyclesOnDemand cycles { function };
        for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
            for (ValueID value : function.blocks[blockIndex].values) {
                const Value& operation = function.at(value);
                std::optional<PropertyKey> named = keyOfSymbolsCreatedBy(function, operation);
                if (std::optional<PropertyKeyID> key = named ? program.types.find(*named) : std::nullopt)
                    found.append(CallNamedByKey { *key, runsAtMostOnce && callsSymbolDirectly(program.builtins(), function, operation) && !cycles.isInCycle(blockIndex) });
            }
        }
    });

    BitVector keysOfCalls(numberOfKeys);
    for (const CallNamedByKey& call : calls) {
        bool isNamedByOtherCall = keysOfCalls.set(call.key.index());
        symbolsCreatedOnce.set(call.key.index(), call.createsOneSymbol && !isNamedByOtherCall);
    }
    return symbolsCreatedOnce;
}

void computeClosuresMadeOnce(const ValueFlowResultInputs& inputs, FlowResultTables& tables)
{
    const Program& program = inputs.program;
    struct ClosureMade {
        FunctionID function;
        bool isMadeAtMostOnce { false };
    };
    Vector<ClosureMade> closures = collectInParallel<ClosureMade>(inputs.loops(), program.functions.size(), [&](unsigned index, Vector<ClosureMade>& found) {
        FunctionID id(index);
        if (!inputs.bodies().holdsAnyOf(id, notedOperations({ ValueFlowNotedOperation::NewClosure })))
            return;
        const Function& function = program.at(id);
        if (!function.hasBody() || (!inputs.isReached(id) && !isUnsolvedCode(inputs, tables, id)))
            return;

        bool runsAtMostOnce = inputs.isReached(id) && tables.functions.functions[index].executions == ExecutionCount::AtMostOnce;
        CyclesOnDemand cycles { function };
        for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
            for (ValueID value : function.blocks[blockIndex].values) {
                if (const Value& operation = function.at(value); operation.opcode == Opcode::NewClosure)
                    found.append(ClosureMade { operation.function(), runsAtMostOnce && !cycles.isInCycle(blockIndex) });
            }
        }
    });

    BitVector isMadeSomewhere(program.functions.size());
    BitVector& hasOneClosure = tables.functions.functionsWithOneClosure;
    hasOneClosure = BitVector(program.functions.size());
    for (const ClosureMade& closure : closures) {
        bool isMadeElsewhere = isMadeSomewhere.set(closure.function.index());
        hasOneClosure.set(closure.function.index(), closure.isMadeAtMostOnce && !isMadeElsewhere);
    }
    tables.functions.functionsWhoseClosuresAreMadeOnce = hasOneClosure;
    for (const Variable& variable : program.variables) {
        if (variable.initialization == VariableInitialization::Function && variable.function) {
            hasOneClosure.clear(variable.function.index());
            tables.functions.functionsWhoseClosuresAreMadeOnce.set(variable.function.index(), !isMadeSomewhere.get(variable.function.index()));
        }
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
