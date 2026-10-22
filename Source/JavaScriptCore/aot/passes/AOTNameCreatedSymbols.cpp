/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTNameCreatedSymbols.h"

#if ENABLE(AOT)

#include "AOTBlockCycles.h"
#include "AOTBodyIndex.h"
#include "AOTBuildContext.h"
#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTable.h"
#include "AOTThreads.h"
#include "AOTValueIdentity.h"
#include <span>

namespace JSC { namespace AOT { namespace Typed {

bool callsSymbolDirectly(const BuiltinTable& builtins, const Function& function, const Value& operation)
{
    if (operation.opcode != Opcode::Call)
        return false;
    const Value& callee = function.at(definitionThroughUpcasts(function, operation.calleeOperand()));
    return callee.opcode == Opcode::Builtin && builtins.canonicalID(callee.builtin()) == Builtins::Symbol;
}

std::optional<PropertyKey> keyOfSymbolsCreatedBy(const Function& function, const Value& operation)
{
    if (operation.opcode != Opcode::Call || !function.module || !operation.hasPosition())
        return std::nullopt;

    std::span<const ValueID> arguments = operation.variadicOperands();
    if (arguments.size() > 1)
        return std::nullopt;

    StringID description;
    if (!arguments.empty()) {
        const Value& argument = function.at(definitionThroughUpcasts(function, arguments[0]));
        if (argument.opcode != Opcode::String)
            return std::nullopt;
        description = argument.string();
    }
    return PropertyKey { .kind = PropertyKeyKind::CreatedSymbol, .text = description, .module = function.module, .position = operation.position };
}

std::optional<PropertyKey> keyOfSymbolRegisteredBy(const BuiltinTable& builtins, const Function& function, const Value& operation)
{
    if (operation.opcode != Opcode::Call)
        return std::nullopt;
    const Value& callee = function.at(definitionThroughUpcasts(function, operation.calleeOperand()));
    if (callee.opcode != Opcode::Builtin || builtins.canonicalID(callee.builtin()) != Builtins::SymbolFor)
        return std::nullopt;

    std::span<const ValueID> arguments = operation.variadicOperands();
    if (arguments.empty())
        return std::nullopt;
    const Value& text = function.at(definitionThroughUpcasts(function, arguments[0]));
    if (text.opcode != Opcode::String)
        return std::nullopt;
    return PropertyKey { .kind = PropertyKeyKind::RegisteredSymbol, .text = text.string() };
}

// A constant text that is handed to a built-in which names a property by it is a name like one that an access spells. Without a key it is a text nobody knows, which may be any name.
static void appendTextsPassedAsKeys(const BuiltinTable& builtins, const Function& function, const Value& operation, Vector<PropertyKey>& keys)
{
    if (operation.opcode != Opcode::Call)
        return;
    const Value& callee = function.at(definitionThroughUpcasts(function, operation.calleeOperand()));
    if (callee.opcode != Opcode::Builtin)
        return;

    std::span<const ValueID> arguments = operation.variadicOperands();
    for (const BuiltinOverload& overload : builtins.overloadsOf(builtins.canonicalID(callee.builtin()))) {
        for (const BuiltinPropertyAction& action : overload.flow.propertyActions) {
            if (action.key.kind != BuiltinKeyKind::Operand || action.key.operand->kind() != BuiltinOperand::Kind::Argument || action.key.operand->argumentIndex() >= arguments.size())
                continue;
            const Value& text = function.at(definitionThroughUpcasts(function, arguments[action.key.operand->argumentIndex()]));
            if (text.opcode == Opcode::String)
                keys.append(PropertyKey { .kind = PropertyKeyKind::Text, .text = text.string() });
        }
    }
}

// The built-ins whose calls give a key: Symbol, Symbol.for, and those that name a property by an argument. By the canonical id.
static BitVector findBuiltinsWhoseCallsGiveKeys(const BuiltinTable& builtins)
{
    BitVector result(builtins.builtinCount());
    for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
        BuiltinID id = builtins.canonicalID(BuiltinID(index));
        bool namesPropertyByArgument = std::ranges::any_of(builtins.overloadsOf(id), [](const BuiltinOverload& overload) {
            return std::ranges::any_of(overload.flow.propertyActions, [](const BuiltinPropertyAction& action) {
                return action.key.kind == BuiltinKeyKind::Operand && action.key.operand->kind() == BuiltinOperand::Kind::Argument;
            });
        });
        if (id == Builtins::Symbol || id == Builtins::SymbolFor || namesPropertyByArgument)
            result.quickSet(id.index());
    }
    return result;
}

// Each of the calls in question calls a built-in that the function itself names. Few functions name one of those, and a function names few built-ins.
static bool namesBuiltinWhoseCallsGiveKeys(const Program& program, FunctionID id, const BitVector& builtinsWhoseCallsGiveKeys)
{
    return anyValueListedOrNot(program, id, { Opcode::Builtin }, [&](ValueID, const Value& value) {
        return builtinsWhoseCallsGiveKeys.quickGet(program.builtins().canonicalID(value.builtin()).index());
    });
}

static void appendKeysOfSymbolsCreatedIn(const Program& program, FunctionID id, const BitVector& builtinsWhoseCallsGiveKeys, Vector<PropertyKey>& keys)
{
    if (!namesBuiltinWhoseCallsGiveKeys(program, id, builtinsWhoseCallsGiveKeys))
        return;
    const BuiltinTable& builtins = program.builtins();
    const Function& function = program.at(id);
    forEachListedValue(program, id, { Opcode::Call }, [&](BlockID, uint32_t, ValueID, const Value& operation) {
        if (std::optional<PropertyKey> key = keyOfSymbolRegisteredBy(builtins, function, operation))
            keys.append(*key);
        appendTextsPassedAsKeys(builtins, function, operation, keys);
    });
    if (!function.module)
        return;

    std::optional<CFG> cfg;
    std::optional<BlockCycles> cycles;
    forEachListedValue(program, id, { Opcode::Call }, [&](BlockID block, uint32_t, ValueID, const Value& operation) {
        if (!callsSymbolDirectly(builtins, function, operation))
            return;
        std::optional<PropertyKey> key = keyOfSymbolsCreatedBy(function, operation);
        if (!key)
            return;
        if (!cycles) {
            cfg.emplace(function);
            cycles.emplace(*cfg);
        }
        if (!cycles->isInCycle(block))
            keys.append(*key);
    });
}

void nameCreatedSymbols(Program& program, BuildContext& build)
{
    RELEASE_ASSERT(program.form() == IRForm::AsRead);

    // They come in the order of the functions, so a key gets the same number on any number of threads.
    // The copies of a function share the positions of its calls, and with them the keys: who may rely on a key counts the calls that it names.
    BitVector builtinsWhoseCallsGiveKeys = findBuiltinsWhoseCallsGiveKeys(program.builtins());
    Vector<PropertyKey> keys = collectInParallel<PropertyKey>(build.parallelLoops(), program.functions.size(), [&](unsigned index, Vector<PropertyKey>& found) {
        appendKeysOfSymbolsCreatedIn(program, FunctionID(index), builtinsWhoseCallsGiveKeys, found);
    });
    for (const PropertyKey& key : keys)
        program.types.intern(key);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
