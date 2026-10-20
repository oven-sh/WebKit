/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSplitStringWalk.h"

#if ENABLE(AOT)

#include "AOTBuiltinCallPlan.h"
#include "AOTBuiltinTable.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static bool isNeverEmptyString(const Program& program, const Type& type)
{
    if (type.isNever() || !type.hasOnlyPrimitives(Primitive::LiteralString))
        return false;
    return std::ranges::none_of(type.stringLiterals, [&](StringID text) {
        return program.strings[text].empty();
    });
}

static bool splitsAtString(const BuiltinTable& builtins, const Value& call)
{
    if (call.opcode != Opcode::CallBuiltin || call.builtinCallForm() != BuiltinCallForm::Whole || !call.receiverOperand() || call.argumentsOperands().size() != 1)
        return false;
    const BuiltinImplementation& implementation = builtins.at(call.overload()).code.implementation;
    return implementation.inlineBuiltin == InlineBuiltin::StringSplit && static_cast<SearchValueKind>(implementation.inlineParameters[0]) == SearchValueKind::String;
}

static BuiltinOverloadID findSeparatorSearch(FunctionPassContext& pass, ValueID string, ValueID separator)
{
    TypingContext typing { pass.program, pass.function, pass.types };
    BuiltinOperandTypes operands { pass.function.at(string).type, { pass.function.at(separator).type, pass.types.intern(Type::integer(Range::length())) } };
    OverloadMatch match = matchOverload({ typing, Builtins::StringPrototypeIndexOf, BuiltinCallMode::Call, operands, operands });
    if (match.state != BuiltinMatchState::Proven || !match.requirements.isEmpty())
        return { };
    return match.overload;
}

std::optional<SplitString> splitStringOf(FunctionPassContext& pass, const Value& call)
{
    const Program& program = pass.program;
    if (!splitsAtString(program.builtins(), call))
        return std::nullopt;

    ValueID string = call.receiverOperand();
    ValueID separator = call.argumentsOperands()[0];
    if (!program.typeOf(pass.function, string).isAnyString() || !isNeverEmptyString(program, program.typeOf(pass.function, separator)))
        return std::nullopt;
    BuiltinOverloadID separatorSearch = findSeparatorSearch(pass, string, separator);
    if (!separatorSearch)
        return std::nullopt;
    return SplitString { string, separator, separatorSearch };
}

Range positionRangeOfSplitString()
{
    // The end of a separator is the sum of an index and a length, and their types only say that each is at most the greatest length.
    return { 0, 2 * Range::length().maximum };
}

SplitStringStep appendSplitStringStep(IRBuilder& builder, const SplitString& splitString, ValueID position)
{
    TypeArena& types = builder.types();
    ValueID length = builder.append(Opcode::GetStringLength, { splitString.string });
    ValueID hasSubstring = builder.append(Opcode::LessEq, { position, length });
    IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(hasSubstring, 0, BlockFrequency::Normal, BlockFrequency::Normal);
    ValueID start = branch.provenOnTaken;

    ValueID found = builder.append(makeBuiltinCall(splitString.separatorSearch, BuiltinCallForm::Whole, { splitString.string, { splitString.separator, start } }, { }));
    ValueID isLast = builder.append(Opcode::Less, { found, builder.appendInteger(0) });

    BlockID merge = builder.addBlock(builder.function().at(builder.currentBlock()).frequency);
    ValueID end = builder.addParameter(merge, types.intern(Type::integer(Range::length())));
    ValueID next = builder.addParameter(merge, types.intern(Type::integer(positionRangeOfSplitString())));
    IRBuilder::ProvenBranch search = builder.emitBranchWithProof(isLast, 0, BlockFrequency::Normal, BlockFrequency::Normal);
    builder.emitJump(merge, { length, builder.append(Opcode::Add, { length, builder.appendInteger(1) }) });

    builder.moveToEndOf(search.notTaken);
    ValueID separatorLength = builder.append(Opcode::GetStringLength, { splitString.separator });
    builder.emitJump(merge, { search.provenOnNotTaken, builder.append(Opcode::Add, { search.provenOnNotTaken, separatorLength }) });

    builder.moveToEndOf(merge);
    return { branch.notTaken, start, end, next };
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
