/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMarkResultsUsedAsKeys.h"

#if ENABLE(AOT)

#include "AOTBlockCycles.h"
#include "AOTBodyIndex.h"
#include "AOTBuildContext.h"
#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTable.h"
#include "AOTCFG.h"
#include "AOTIRProgram.h"
#include "AOTNameCreatedSymbols.h"
#include "AOTSort.h"
#include "AOTStringBuilderAnalysis.h"
#include "AOTThreads.h"
#include "AOTUseLists.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include <algorithm>
#include <span>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

// What a function returns: strings that it makes, and results of functions that it calls.
struct ReturnedStrings {
    FunctionID function;
    Vector<ValueID, 2> made;
    Vector<FunctionID, 2> forwarded;
};

struct KeyResultUse {
    FunctionID callee;
    FunctionID caller;
    bool isKey { false };
    bool isPartOfKey { false };
    bool isReturned { false };
};

static ReturnedStrings findReturnedStrings(const Program& program, FunctionID id)
{
    const Function& function = program.at(id);
    ReturnedStrings result { id, { }, { } };
    BitVector isVisited;
    Vector<ValueID, 8> worklist;
    auto add = [&](ValueID value) {
        if (value && !isVisited.set(value.index()))
            worklist.append(value);
    };
    auto addArgumentsFor = [&](ValueID parameter, const Target& target) {
        const auto& parameters = function.at(target.block).parameters;
        for (uint32_t index = 0; index < target.arguments.size(); ++index) {
            if (parameters[index] == parameter)
                add(target.arguments[index]);
        }
    };
    for (const Block& block : function.blocks) {
        if (block.terminator.kind == TerminatorKind::Return)
            add(block.terminator.valueOperand());
        else if (block.terminator.kind == TerminatorKind::ReturnCall)
            add(block.terminator.callOperand());
    }
    while (!worklist.isEmpty()) {
        ValueID returned = worklist.takeLast();
        const Value& value = function.at(returned);
        if (ValueID definition = sameObjectThroughCasts(function, returned); definition != returned)
            add(definition);
        else if (value.opcode == Opcode::Parameter) {
            for (const Block& block : function.blocks) {
                for (const Target& target : block.terminator.targets)
                    addArgumentsFor(returned, target);
                if (block.handler)
                    addArgumentsFor(returned, *block.handler);
            }
        } else if (value.opcode == Opcode::CallDirect)
            result.forwarded.append(value.function());
        else if (canBeMarkedAsLikelyKey(program, function, value))
            result.made.append(returned);
    }
    return result;
}

static KeyResultUse findUseOfResult(const Program& program, FunctionID id, const UseLists& useLists, ValueID call)
{
    struct Text {
        ValueID value;
        bool isPart;
    };
    const Function& caller = program.at(id);
    KeyResultUse result { caller.at(call).function(), id };
    BitVector isVisited;
    Vector<Text, 8> worklist { Text { call, false } };
    isVisited.set(call.index());
    while (!worklist.isEmpty()) {
        Text text = worklist.takeLast();
        for (const Use& use : useLists.usesOf(text.value)) {
            Text next { { }, text.isPart };
            if (isKeyOperand(program, caller, use))
                (text.isPart ? result.isPartOfKey : result.isKey) = true;
            else if (use.kind == Use::Kind::TerminatorOperand) {
                TerminatorKind kind = caller.at(use.block).terminator.kind;
                result.isReturned |= !text.isPart && (kind == TerminatorKind::Return || kind == TerminatorKind::ReturnCall);
            } else if (use.kind == Use::Kind::EdgeArgument)
                next.value = caller.at(targetOf(caller, use.edge()).block).parameters[use.operandIndex];
            else if (use.kind == Use::Kind::Operand && isCast(caller.at(use.user).opcode))
                next.value = use.user;
            else if (use.kind == Use::Kind::Operand && isStringConcatenation(program, caller, caller.at(use.user)))
                next = { use.user, true };
            if (next.value && !isVisited.set(next.value.index()))
                worklist.append(next);
        }
    }
    return result;
}

void markResultsUsedAsKeys(ProgramEditor& editor, BuildContext& build)
{
    const Program& program = editor.program();
    unsigned numberOfFunctions = program.functions.size();
    Vector<ReturnedStrings> returned = collectInParallel<ReturnedStrings>(build.parallelLoops(), numberOfFunctions, [&](unsigned index, Vector<ReturnedStrings>& found) {
        const Function& function = program.functions[index];
        if (!function.hasBody() || function.results.size() != 1 || !(program.at(function.results[0].type).primitives & Primitive::AnyString))
            return;
        ReturnedStrings strings = findReturnedStrings(program, FunctionID(index));
        if (!strings.made.isEmpty() || !strings.forwarded.isEmpty())
            found.append(WTF::move(strings));
    });

    // A function is of interest where it makes a string, or hands on the result of one that is.
    BitVector isCandidate(numberOfFunctions);
    for (const ReturnedStrings& strings : returned) {
        if (!strings.made.isEmpty())
            isCandidate.quickSet(strings.function.index());
    }
    if (isCandidate.isEmpty())
        return;
    for (bool didChange = true; std::exchange(didChange, false);) {
        for (const ReturnedStrings& strings : returned) {
            if (isCandidate.quickGet(strings.function.index()))
                continue;
            if (std::ranges::any_of(strings.forwarded, [&](FunctionID callee) { return isCandidate.quickGet(callee.index()); })) {
                isCandidate.quickSet(strings.function.index());
                didChange = true;
            }
        }
    }

    Vector<KeyResultUse> uses = collectInParallel<KeyResultUse>(build.parallelLoops(), numberOfFunctions, [&](unsigned index, Vector<KeyResultUse>& found) {
        Vector<ValueID, 4> calls;
        forEachDirectCall(program, FunctionID(index), [&](BlockID, ValueID value, FunctionID callee) {
            if (isCandidate.get(callee.index()))
                calls.append(value);
        });
        if (calls.isEmpty())
            return;

        CFG cfg(program.functions[index]);
        UseLists useLists(program.functions[index], cfg);
        for (ValueID call : calls) {
            if (cfg.isReachable(useLists.blockOf(call)) && !useLists.usesOf(call).empty())
                found.append(findUseOfResult(program, FunctionID(index), useLists, call));
        }
    });
    AOT::stableSort(uses, { }, &KeyResultUse::callee);

    uint64_t numberOfMarkedStrings = 0;
    for (const ReturnedStrings& strings : returned) {
        if (strings.made.isEmpty() || !build.isEnabled(PassID::MarkResultsUsedAsKeys, strings.function))
            continue;
        // A caller that only returns the result stands for its own callers, and for nobody where no direct call of it is known: an entrypoint for calls by value, an export.
        unsigned keys = 0;
        unsigned partsOfKeys = 0;
        unsigned others = 0;
        BitVector isVisited;
        Vector<FunctionID, 8> worklist { strings.function };
        isVisited.set(strings.function.index());
        while (!worklist.isEmpty()) {
            for (const KeyResultUse& use : std::ranges::equal_range(uses, worklist.takeLast(), { }, &KeyResultUse::callee)) {
                if (use.isKey)
                    ++keys;
                else if (use.isPartOfKey)
                    ++partsOfKeys;
                else if (!use.isReturned)
                    ++others;
                else if (!isVisited.set(use.caller.index()))
                    worklist.append(use.caller);
            }
        }
        // A rope serves as well as a part of a key: its characters are copied and it is never flattened. What a builder makes is flat anyway, and as a key it is shared with equal texts.
        bool marksConcatenations = keys > partsOfKeys + others;
        bool marksBuilders = keys + partsOfKeys > others;
        for (ValueID string : strings.made) {
            bool isConcatenation = isStringConcatenation(program, program.at(strings.function), program.at(strings.function).at(string));
            if (!(isConcatenation ? marksConcatenations : marksBuilders))
                continue;
            markAsLikelyKey(editor.atForLowering(strings.function), string);
            ++numberOfMarkedStrings;
        }
    }
    build.statistics().count(StatisticID::ResultsMarkedAsKeys, numberOfMarkedStrings);
}

// By the canonical id. Nothing where the operation is no call, or calls something else.
static BuiltinID builtinCalledBy(const BuiltinTable& builtins, const Function& function, const Value& operation)
{
    if (operation.opcode != Opcode::Call)
        return { };
    const Value& callee = function.at(definitionThroughUpcasts(function, operation.calleeOperand()));
    return callee.opcode == Opcode::Builtin ? builtins.canonicalID(callee.builtin()) : BuiltinID();
}

static const Value* stringPassedAs(const Function& function, ValueID argument)
{
    const Value& text = function.at(definitionThroughUpcasts(function, argument));
    return text.opcode == Opcode::String ? &text : nullptr;
}

static bool namesPropertyByArgument(const BuiltinPropertyAction& action)
{
    return action.key.kind == BuiltinKeyKind::Operand && action.key.operand->kind() == BuiltinOperand::Kind::Argument;
}

bool callsSymbolDirectly(const BuiltinTable& builtins, const Function& function, const Value& operation)
{
    return builtinCalledBy(builtins, function, operation) == Builtins::Symbol;
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
        const Value* text = stringPassedAs(function, arguments[0]);
        if (!text)
            return std::nullopt;
        description = text->string();
    }
    return PropertyKey { .kind = PropertyKeyKind::CreatedSymbol, .text = description, .module = function.module, .position = operation.position };
}

std::optional<PropertyKey> keyOfSymbolRegisteredBy(const BuiltinTable& builtins, const Function& function, const Value& operation)
{
    if (builtinCalledBy(builtins, function, operation) != Builtins::SymbolFor || operation.variadicOperands().empty())
        return std::nullopt;
    const Value* text = stringPassedAs(function, operation.variadicOperands()[0]);
    if (!text)
        return std::nullopt;
    return PropertyKey { .kind = PropertyKeyKind::RegisteredSymbol, .text = text->string() };
}

static constexpr OpcodeSet accessesByKey { Opcode::ElemGet, Opcode::ElemSet, Opcode::ElemHas, Opcode::ElemDelete, Opcode::ElemDefine };

static bool isUsedOnlyAsKey(const Function& function, const UseLists& useLists, ValueID value)
{
    return std::ranges::all_of(useLists.usesOf(value), [&](const Use& use) {
        if (use.kind != Use::Kind::Operand)
            return false;
        const Value& user = function.at(use.user);
        if (isCast(user.opcode))
            return isUsedOnlyAsKey(function, useLists, use.user);
        return accessesByKey.contains(user.opcode) && use.operandIndex == OperandPosition::property(user.opcode);
    });
}

static VariableID findOnlyVariableStoredWith(const Function& function, const UseLists& useLists, ValueID value)
{
    std::span<const Use> uses = useLists.usesOf(value);
    if (uses.size() != 1 || uses[0].kind != Use::Kind::Operand)
        return { };
    const Value& user = function.at(uses[0].user);
    if (isCast(user.opcode))
        return findOnlyVariableStoredWith(function, useLists, uses[0].user);
    return user.opcode == Opcode::PutModuleVariable ? user.variable() : VariableID();
}

BitVector findLibrarySymbolsUsedOnlyAsKeys(const Program& program)
{
    BitVector symbols(program.types.numberOfPropertyKeys());
    BitVector modules(program.modules.size());
    for (uint32_t index = 0; index < program.types.numberOfPropertyKeys(); ++index) {
        const PropertyKey& key = program.at(PropertyKeyID(index));
        if (key.kind != PropertyKeyKind::CreatedSymbol || !key.module)
            continue;
        const Module& module = program.at(key.module);
        if (module.kind == ModuleKind::RuntimeSource || module.flags.contains(ModuleFlags::IsLibrary))
            modules.set(key.module.index());
    }
    if (modules.isEmpty())
        return symbols;

    struct UsesOnDemand {
        const Function& function;
        std::optional<CFG> cfg { };
        std::optional<UseLists> useLists { };

        const UseLists& get()
        {
            if (!useLists) {
                cfg.emplace(function);
                useLists.emplace(function, *cfg);
            }
            return *useLists;
        }
    };
    auto forEachValueInModules = [&](NOESCAPE const Invocable<void(const Function&, UsesOnDemand&, ValueID, const Value&)> auto& functor) {
        for (const Function& function : program.functions) {
            if (!function.hasBody() || !function.module || !modules.get(function.module.index()))
                continue;
            UsesOnDemand uses { function };
            for (const Block& block : function.blocks) {
                for (ValueID id : block.values)
                    functor(function, uses, id, function.at(id));
            }
        }
    };

    struct HeldSymbol {
        VariableID variable;
        PropertyKeyID key;
    };
    Vector<HeldSymbol> held;
    BitVector holdsSymbol(program.variables.size());
    forEachValueInModules([&](const Function& function, UsesOnDemand& uses, ValueID id, const Value& value) {
        if (value.opcode != Opcode::Call || !callsSymbolDirectly(program.builtins(), function, value))
            return;
        std::optional<PropertyKey> named = keyOfSymbolsCreatedBy(function, value);
        std::optional<PropertyKeyID> key = named ? program.types.find(*named) : std::nullopt;
        VariableID variable = key ? findOnlyVariableStoredWith(function, uses.get(), id) : VariableID();
        if (!variable)
            return;
        held.append({ variable, *key });
        holdsSymbol.set(variable.index());
    });

    BitVector isStoredTo(program.variables.size());
    BitVector leavesVariable(program.variables.size());
    for (size_t index : modules) {
        const Module& module = program.modules[index];
        for (const ModuleExport& entry : module.exports) {
            if (entry.variable)
                leavesVariable.set(entry.variable.index());
        }
        for (const SyntheticExport& entry : module.syntheticExports) {
            if (entry.variable)
                leavesVariable.set(entry.variable.index());
        }
    }
    forEachValueInModules([&](const Function& function, UsesOnDemand& uses, ValueID id, const Value& value) {
        if ((value.opcode != Opcode::GetModuleVariable && value.opcode != Opcode::PutModuleVariable) || !holdsSymbol.get(value.variable().index()))
            return;
        uint32_t variable = value.variable().index();
        if (value.opcode == Opcode::PutModuleVariable ? isStoredTo.set(variable) : !isUsedOnlyAsKey(function, uses.get(), id))
            leavesVariable.set(variable);
    });
    for (const HeldSymbol& symbol : held) {
        if (!leavesVariable.get(symbol.variable.index()))
            symbols.set(symbol.key.index());
    }
    return symbols;
}

void nameCreatedSymbols(Program& program, BuildContext& build)
{
    RELEASE_ASSERT(program.form() == IRForm::AsRead);
    const BuiltinTable& builtins = program.builtins();

    // The built-ins whose calls give a key: Symbol, Symbol.for, and those that name a property by an argument. By the canonical id.
    BitVector builtinsWhoseCallsGiveKeys(builtins.builtinCount());
    for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
        BuiltinID id = builtins.canonicalID(BuiltinID(index));
        bool namesProperty = std::ranges::any_of(builtins.overloadsOf(id), [](const BuiltinOverload& overload) {
            return std::ranges::any_of(overload.flow.propertyActions, namesPropertyByArgument);
        });
        if (id == Builtins::Symbol || id == Builtins::SymbolFor || namesProperty)
            builtinsWhoseCallsGiveKeys.quickSet(id.index());
    }

    // They come in the order of the functions, so a key gets the same number on any number of threads.
    Vector<PropertyKey> keys = collectInParallel<PropertyKey>(build.parallelLoops(), program.functions.size(), [&](unsigned index, Vector<PropertyKey>& found) {
        FunctionID id(index);
        const Function& function = program.at(id);
        // Each of the calls in question calls a built-in that the function itself names. Few functions name one of those, and a function names few built-ins.
        bool namesSuchBuiltin = anyValueListedOrNot(program, id, { Opcode::Builtin }, [&](ValueID, const Value& value) {
            return builtinsWhoseCallsGiveKeys.quickGet(builtins.canonicalID(value.builtin()).index());
        });
        if (!namesSuchBuiltin)
            return;

        std::optional<CFG> cfg;
        std::optional<BlockCycles> cycles;
        Vector<PropertyKey> createdSymbols;
        forEachListedValue(program, id, { Opcode::Call }, [&](BlockID block, uint32_t, ValueID, const Value& operation) {
            BuiltinID callee = builtinCalledBy(builtins, function, operation);
            if (!callee)
                return;
            if (std::optional<PropertyKey> key = keyOfSymbolRegisteredBy(builtins, function, operation))
                found.append(*key);

            // A constant text that is handed to a built-in which names a property by it is a name like one that an access spells. Without a key it is a text nobody knows, which may be any name.
            std::span<const ValueID> arguments = operation.variadicOperands();
            for (const BuiltinOverload& overload : builtins.overloadsOf(callee)) {
                for (const BuiltinPropertyAction& action : overload.flow.propertyActions) {
                    if (!namesPropertyByArgument(action) || action.key.operand->argumentIndex() >= arguments.size())
                        continue;
                    if (const Value* text = stringPassedAs(function, arguments[action.key.operand->argumentIndex()]))
                        found.append(PropertyKey { .kind = PropertyKeyKind::Text, .text = text->string() });
                }
            }

            std::optional<PropertyKey> key = callee == Builtins::Symbol ? keyOfSymbolsCreatedBy(function, operation) : std::nullopt;
            if (!key)
                return;
            if (!cycles) {
                cfg.emplace(function);
                cycles.emplace(*cfg);
            }
            if (!cycles->isInCycle(block))
                createdSymbols.append(*key);
        });
        found.appendVector(createdSymbols);
    });
    for (const PropertyKey& key : keys)
        program.types.intern(key);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
