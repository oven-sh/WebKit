/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowSymbols.h"

#if ENABLE(AOT)

#include "AOTFlowPropertyState.h"
#include "AOTValueIdentity.h"
#include <algorithm>
#include <tuple>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

static std::pair<ModuleID, uint32_t> creationPointOf(const FlowSymbolKeyAsRead& entry)
{
    return { entry.module, entry.position };
}

FlowSymbolState createFlowSymbolState(const Program& program)
{
    FlowSymbolState state;
    for (uint32_t index = 0, count = program.types.numberOfPropertyKeys(); index < count; ++index) {
        PropertyKeyID id(index);
        const PropertyKey& key = program.at(id);
        if (key.kind == PropertyKeyKind::CreatedSymbol && key.module && key.position)
            state.keysAsRead.append(FlowSymbolKeyAsRead { key.module, *key.position, id });
    }
    std::ranges::sort(state.keysAsRead, { }, [](const FlowSymbolKeyAsRead& entry) {
        return std::tuple { entry.module, entry.position, entry.key };
    });
    return state;
}

static PropertyKeyID findKeyAsRead(const FlowSymbolState& state, ModuleID module, uint32_t position)
{
    std::pair<ModuleID, uint32_t> creationPoint { module, position };
    auto found = std::ranges::lower_bound(state.keysAsRead, creationPoint, { }, creationPointOf);
    if (found == state.keysAsRead.end() || creationPointOf(*found) != creationPoint)
        return { };
    return found->key;
}

static bool callsSymbolDirectly(const BuiltinTable& builtins, const Function& function, const Value& operation)
{
    if (operation.opcode != Opcode::Call)
        return false;
    const Value& callee = function.at(definitionThroughUpcasts(function, operation.calleeOperand()));
    return callee.opcode == Opcode::Builtin && builtins.canonicalID(callee.builtin()) == Builtins::Symbol;
}

static StringID constantDescriptionOf(const Function& function, const Value& call)
{
    std::span<const ValueID> arguments = call.variadicOperands();
    if (arguments.empty())
        return { };
    const Value& description = function.at(definitionThroughUpcasts(function, arguments[0]));
    if (description.opcode != Opcode::String)
        return { };
    return description.string();
}

static void recordCreation(FlowSymbolState& state, const FlowSymbolCreation& creation)
{
    auto result = state.creationIndexes.add(creation.key, static_cast<uint32_t>(state.creations.size()));
    if (result.isNewEntry) {
        state.creations.append(creation);
        return;
    }
    FlowSymbolCreation& recorded = state.creations[result.iterator->value];
    recorded.isInCycle |= creation.isInCycle || recorded.function != creation.function || recorded.call != creation.call;
}

FlowValue valueOfCreatedSymbol(FlowSolver& solver, const FlowSymbolCall& call)
{
    FlowValueTable& values = solver.values();
    if (!call.site.instance || solver.graph().isHost(call.site.instance) || !call.origin)
        return values.valueOfPrimitives(Primitive::Symbol);

    const Function& function = solver.functionOf(call.site);
    const Value& operation = function.at(call.origin);
    if (!function.module || !operation.hasPosition())
        return values.valueOfPrimitives(Primitive::Symbol);

    FlowSymbolState& state = solver.propertyState().symbols;
    bool isDirectCall = callsSymbolDirectly(solver.builtins(), function, operation);
    PropertyKeyID key = findKeyAsRead(state, function.module, operation.position);
    if (!key) {
        key = solver.types().intern(PropertyKey {
            .kind = PropertyKeyKind::CreatedSymbol,
            .text = isDirectCall ? constantDescriptionOf(function, operation) : StringID(),
            .module = function.module,
            .position = operation.position,
        });
    }

    // What reaches Symbol through another function may call it several times, with a description this operation does not show.
    recordCreation(state, { key, solver.graph().at(call.site.instance).function, call.origin, call.isInCycle || !isDirectCall });
    return values.valueOfSymbol(key);
}

FlowValue valueOfRegisteredSymbols(FlowSolver& solver, const FlowValue& registryKeys)
{
    constexpr uint16_t namedRegistryKeys = Primitive::LiteralString | Primitive::Absent;

    FlowValueTable& values = solver.values();
    FlowValue result;
    for (StringID string : values.stringsOf(registryKeys.strings))
        values.join(result, values.valueOfSymbol(solver.types().registeredSymbolKey(string)), Subject { });
    if ((registryKeys.primitives & ~namedRegistryKeys) || registryKeys.mayBeObject())
        values.join(result, values.valueOfPrimitives(Primitive::Symbol), Subject { });
    return result;
}

std::span<const FlowSymbolCreation> symbolCreationsOf(const FlowSolution& solution)
{
    return solution.propertyState.symbols.creations.span();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
