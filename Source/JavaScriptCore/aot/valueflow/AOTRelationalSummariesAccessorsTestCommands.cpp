/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowTestCommands.h"

#if ENABLE(AOT)

#include "AOTFlowApply.h"
#include "AOTIRPrinter.h"
#include "AOTRelationalSummaries.h"
#include "AOTSupportTestCommands.h"
#include "AOTValueFlowResults.h"
#include "AOTVector.h"
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT { namespace Typed {

static LinkedFate fateOf(ValueFate access)
{
    LinkedFate fate = LinkedFate::nothing();
    fate.itself = access;
    return fate;
}

static LinkedFate fateOfAccess(ValueFate access, const KeysOfMembers& keys)
{
    LinkedFate fate = fateOf(access);
    (access == ValueFate::HasMembersRead ? fate.keysRead : fate.keysWritten) = keys;
    return fate;
}

static PropertyKeyID findKey(const Program& program, const String& word)
{
    const BuiltinTable& table = program.builtins();
    if (word == "@@iterator"_s)
        return program.keyOf(table.nameOf(WellKnownSymbol::Iterator));
    if (word == "@@toPrimitive"_s)
        return program.keyOf(table.nameOf(WellKnownSymbol::ToPrimitive));
    // symbol:<description>: the key that flow has made of the symbols which one call of Symbol creates.
    constexpr ASCIILiteral symbolPrefix = "symbol:"_s;
    bool isSymbol = word.startsWith(symbolPrefix);
    StringID text = program.strings.find(byteCast<uint8_t>((isSymbol ? word.substring(symbolPrefix.length()) : word).utf8().span()));
    if (!text)
        return { };
    if (isSymbol) {
        for (uint32_t index = 0; index < program.types.numberOfPropertyKeys(); ++index) {
            const PropertyKey& key = program.at(PropertyKeyID(index));
            if (key.kind == PropertyKeyKind::CreatedSymbol && key.text == text)
                return PropertyKeyID(index);
        }
        return { };
    }
    return program.types.find(PropertyKey { .kind = PropertyKeyKind::Text, .text = text }).value_or(PropertyKeyID());
}

// runs-no-code <key>..: flow's results are applied, then one line for each operation that makes an object and for each entry parameter at which only objects arrive.
void runRunsNoCodeTestCommand(TestCommandContext& context)
{
    Program& program = *context.program;
    FlowResults flow = analyzeValueFlow(program, program.types, context.build);
    applyFlowResults(program, flow, context.build);
    program.setForm(IRForm::Prepared);

    Vector<PropertyKeyID> keys;
    for (const String& word : context.arguments) {
        PropertyKeyID key = findKey(program, word);
        if (!key)
            return printBadArgument(context.out, word);
        keys.append(key);
    }

    PrintStream& out = context.out;
    auto printAnswers = [&](NOESCAPE const Invocable<std::optional<GettersOfProgram>(const LinkedFate&)> auto& ask) {
        auto answer = [&](const LinkedFate& fate) {
            std::optional<GettersOfProgram> getters = ask(fate);
            if (!getters || getters->isEmpty())
                return makeString(yesOrNo(!!getters));
            return makeString(interleave(*getters, [](StringBuilder& builder, FunctionID getter) {
                builder.append('f', getter.index());
            }, '+'));
        };
        for (size_t position = 0; position < keys.size(); ++position) {
            KeysOfMembers key;
            key.add(keys[position]);
            out.print(" "_s, context.arguments[position], "="_s, answer(fateOfAccess(ValueFate::HasMembersRead, key)), "|"_s, answer(fateOfAccess(ValueFate::HasMembersWritten, key)));
        }
        out.print(" any="_s, answer(fateOfAccess(ValueFate::HasMembersRead, KeysOfMembers::any())), "|"_s, answer(fateOfAccess(ValueFate::HasMembersWritten, KeysOfMembers::any())));
        out.print(" keys="_s, answer(fateOf(ValueFate::HasKeysEnumerated)), " iterate="_s, answer(fateOf(ValueFate::IsIterated)), " convert="_s, answer(fateOf(ValueFate::IsConvertedToPrimitive)));
        out.println(" define="_s, answer(fateOf(ValueFate::HasMembersDeletedOrDefined)), " unlisted="_s, answer(fateOf(ValueFate::HasMembersRead)), " called="_s, answer(fateOf(ValueFate::IsCalled)));
    };
    auto isAsked = [&](TypeID type) {
        return type && !program.at(type).primitives && (program.at(type).hasObjects() || program.at(type).environment);
    };
    auto printAnswersForValue = [&](uint32_t function, ValueID id, TypeID type) {
        if (!isAsked(type))
            return;
        out.print("f"_s, function, " v"_s, id.index(), ": "_s);
        printType(out, program, type);
        out.print(":"_s);
        printAnswers([&](const LinkedFate& fate) {
            std::optional<GettersOfProgram> getters = gettersRunOn(program, flow, program.at(type), fate);
            RELEASE_ASSERT(runsNoCodeOn(program, flow, program.at(type), fate) == (getters && getters->isEmpty()));
            return getters;
        });
        out.print("f"_s, function, " v"_s, id.index(), " stores land:"_s);
        for (size_t position = 0; position < keys.size(); ++position) {
            KeysOfMembers key;
            key.add(keys[position]);
            out.print(" "_s, context.arguments[position], "="_s, yesOrNo(storesLandInOwnFields(program, flow, program.at(type), fateOfAccess(ValueFate::HasMembersWritten, key))));
        }
        out.println(" any="_s, yesOrNo(storesLandInOwnFields(program, flow, program.at(type), fateOfAccess(ValueFate::HasMembersWritten, KeysOfMembers::any()))));
    };

    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        const Function& function = program.at(FunctionID(index));
        if (!function.hasBody())
            continue;
        for (ValueID parameter : function.blocks[0].parameters)
            printAnswersForValue(index, parameter, function.at(parameter).type);
        ProofsBehindValues proofs(function);
        Vector<ValueID> returnedValues;
        for (const Block& block : function.blocks) {
            if (block.terminator.kind == TerminatorKind::Return) {
                block.terminator.forEachOperand([&](ValueID operand, uint32_t) {
                    while (operand && interpretedOf(function.at(operand).opcode) == Interpretation::Cast)
                        operand = function.at(operand).operand(0);
                    returnedValues.append(operand);
                });
            }
        }
        for (const Block& block : function.blocks) {
            for (ValueID id : block.values) {
                const Value& value = function.at(id);
                if (interpretedOf(value.opcode) == Interpretation::Allocation)
                    printAnswersForValue(index, id, value.type);
                bool isCall = effectRuleOf(value.opcode) == EffectRule::Call;
                if ((isCall || interpretedOf(value.opcode) == Interpretation::Allocation) && value.type && program.at(value.type).hasObjects()) {
                    bool isReturned = returnedValues.contains(id);
                    out.print("f"_s, index, " v"_s, id.index(), isReturned ? " made and returned:"_s : " made:"_s);
                    printAnswers([&](LinkedFate fate) {
                        if (isReturned) {
                            fate.itself.add(ValueFate::IsReturned);
                            fate.results = 1;
                        }
                        std::optional<GettersOfProgram> getters = gettersRunOnObjectMadeBy(program, flow, function, proofs, id, fate);
                        RELEASE_ASSERT(runsNoCodeOnObjectMadeBy(program, flow, function, proofs, id, fate) == (getters && getters->isEmpty()));
                        return getters;
                    });
                }
                if (!isCall)
                    continue;
                value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                    if (!operand || !value.variadicOperandIndex(operandIndex) || !isAsked(function.at(operand).type))
                        return;
                    out.print("f"_s, index, " v"_s, id.index(), " operand "_s, operandIndex, ":"_s);
                    printAnswers([&](const LinkedFate& fate) {
                        std::optional<GettersOfProgram> getters = gettersRunOnOperandAt(program, flow, function, value, operandIndex, fate);
                        RELEASE_ASSERT(runsNoCodeOnOperandAt(program, flow, function, value, operandIndex, fate) == (getters && getters->isEmpty()));
                        return getters;
                    });
                });
            }
        }
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
