/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSimplifyBuiltinCallsInternal.h"

#if ENABLE(AOT)

#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include "AOTSplitUnions.h"
#include "AOTStringPositions.h"
#include "AOTStringRanges.h"
#include "PureNaN.h"
#include <ranges>
#include <span>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct ReadRewrite {
    ValueID original;
    Value rewritten;
};

struct CodeUnitRead {
    ValueSite site;
    ValueID string;
    ValueID position;
    bool readsCodePoint { false };
    bool isInside { false };
};

// The position is the one of the call, as narrowed as the tests in front of the block have proven it.
struct BlockWithCall {
    BlockID block;
    ValueID position;
};

} // anonymous namespace

static constexpr int64_t surrogateKindMask = 0xFC00;
static constexpr int64_t leadSurrogateKind = 0xD800;

static std::optional<CodeUnitRead> codeUnitReadOf(FunctionPassContext& context, const ValueSite& site, const Value& call)
{
    const Program& program = context.program;
    BuiltinID builtin = program.builtins().at(call.overload()).builtin;
    if (builtin != Builtins::StringPrototypeCharCodeAt && builtin != Builtins::StringPrototypeCodePointAt)
        return std::nullopt;

    ValueID string = call.receiverOperand();
    std::span<const ValueID> arguments = call.argumentsOperands();
    if (!string || arguments.size() != 1 || !program.typeOf(context.function, string).isAnyString())
        return std::nullopt;
    // A number that is never an integer always takes the call.
    const Type& position = program.typeOf(context.function, arguments[0]);
    if (!position.isNumber() || !(position.primitives & Primitive::Integer))
        return std::nullopt;
    // Selection reads the characters of a range in the string it was cut from, and knows the call for that.
    if (stringRangeSourceOf(program, context.function, context.function.at(context.analyses.sameObjectThroughCasts(string))))
        return std::nullopt;
    return CodeUnitRead { .site = site, .string = string, .position = arguments[0], .readsCodePoint = builtin == Builtins::StringPrototypeCodePointAt };
}

// Inside the string the answer is an integer, outside it is NaN or undefined.
static void expandIntoTwoPaths(SiteExpansion& expansion, const CodeUnitRead& read)
{
    IRBuilder& builder = expansion.builder();
    expansion.begin(read.site);
    BlockFrequency frequency = expansion.frequency();
    BlockFrequency frequencyOutside = rarerOf(frequency, BlockFrequency::Unlikely);

    auto typeOf = [&](ValueID id) -> const Type& {
        return expansion.context().program.typeOf(expansion.function(), id);
    };
    // A position that lies outside every string has the type never on the path inside, and nothing is computed from a value that cannot exist.
    auto mayBeInside = [&](ValueID position) {
        return expansion.function().at(position).type != TypeTable::never;
    };

    Vector<BlockWithCall, 2> blocksWithCall;
    Vector<BlockID, 2> blocksOutside;
    ValueID position = read.position;
    if (!typeOf(position).isInteger()) {
        ValueID isInt32 = builder.appendTypeTest(position, TypeTable::int32);
        IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(isInt32, OperandIndex::TypeTest::value, frequency, frequencyOutside);
        blocksWithCall.append(BlockWithCall { branch.notTaken, branch.provenOnNotTaken });
        position = branch.provenOnTaken;
    }
    // Read as an unsigned number a negative int32 lies above every length, so the one test against the length sends it outside too.
    if (!read.isInside && typeOf(position).integers.minimum < 0 && typeOf(position).integers.maximum >= 0 && typeOf(position).integers.isSubsetOf(Range::int32()))
        position = expansion.append(IRBuilder::make(Opcode::URShift, { position, builder.appendInteger(0) }));
    else if (!read.isInside && typeOf(position).integers.minimum < 0) {
        ValueID isNotNegative = expansion.append(IRBuilder::make(Opcode::LessEq, { builder.appendInteger(0), position }));
        IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(isNotNegative, OperandIndex::LessEq::right, frequency, frequencyOutside);
        blocksOutside.append(branch.notTaken);
        position = branch.provenOnTaken;
    }
    if (!read.isInside && mayBeInside(position)) {
        ValueID length = expansion.append(IRBuilder::make(Opcode::GetStringLength, { read.string }));
        ValueID isBelowLength = expansion.append(IRBuilder::make(Opcode::Less, { position, length }));
        IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(isBelowLength, OperandIndex::Less::left, frequency, frequencyOutside);
        blocksOutside.append(branch.notTaken);
        position = branch.provenOnTaken;
    }
    if (mayBeInside(position)) {
        ValueID unit = expansion.append(IRBuilder::make(Opcode::GetStringCodeUnit, { read.string, position }));
        if (read.readsCodePoint) {
            // Nearly every unit lies below the surrogates: one comparison says so. Only the others are asked which kind they are.
            ValueID isBelowSurrogates = expansion.append(IRBuilder::make(Opcode::Less, { unit, builder.appendInteger(leadSurrogateKind) }));
            IRBuilder::BranchBlocks below = builder.emitBranch(isBelowSurrogates, frequency, frequencyOutside);
            builder.moveToEndOf(below.taken);
            expansion.finishArm(unit);
            builder.moveToEndOf(below.notTaken);
            frequency = frequencyOutside;
            ValueID kind = expansion.append(IRBuilder::make(Opcode::BitAnd, { unit, builder.appendInteger(surrogateKindMask) }));
            ValueID isLead = expansion.append(IRBuilder::make(Opcode::StrictEq, { kind, builder.appendInteger(leadSurrogateKind) }));
            IRBuilder::BranchBlocks blocks = builder.emitBranch(isLead, frequencyOutside, frequency);
            blocksWithCall.append(BlockWithCall { blocks.taken, position });
            builder.moveToEndOf(blocks.notTaken);
        }
        expansion.finishArm(unit);
    } else
        expansion.finishArmAsUnreachable();

    for (BlockID block : blocksOutside) {
        builder.moveToEndOf(block);
        expansion.finishArm(read.readsCodePoint ? builder.appendUndefined() : builder.appendDouble(PNaN));
    }
    // A copy of the call is not expanded again: only a call in a block of normal frequency is.
    for (const BlockWithCall& withCall : blocksWithCall) {
        builder.moveToEndOf(withCall.block);
        expansion.finishArm(expansion.appendOriginal({ .operandIndex = OperandIndex::CallBuiltin::arguments, .operand = withCall.position, .isNarrowedValue = true }));
    }
    expansion.end();
}

unsigned simplifyStringReads(FunctionPassContext& context, const BuiltinCallSites& calls)
{
    StringPositions positions(context.analyses);
    Vector<ReadRewrite> rewrites;
    Vector<CodeUnitRead> readsWithTwoPaths;
    auto rewrite = [&](ValueID original, Value&& rewritten) {
        if (keepsTypeOf(context, original, rewritten))
            rewrites.append(ReadRewrite { original, WTF::move(rewritten) });
    };
    for (const ValueSite& site : calls) {
        const Value& call = context.function.at(site.value);
        if (call.builtinCallForm() != BuiltinCallForm::Whole)
            continue;
        if (ValueID string = stringMeasuredBy(context.program, context.function, call)) {
            rewrite(site.value, IRBuilder::make(Opcode::GetStringLength, { string }));
            continue;
        }
        std::optional<CodeUnitRead> read = codeUnitReadOf(context, site, call);
        if (!read)
            continue;
        read->isInside = context.program.typeOf(context.function, read->position).isInteger() && positions.isInside(read->string, read->position, site.block);
        // Inside the string a code unit is the whole answer. A code point may take the unit behind it too.
        if (read->isInside && !read->readsCodePoint)
            rewrite(site.value, IRBuilder::make(Opcode::GetStringCodeUnit, { read->string, read->position }));
        else if (context.function.at(site.block).frequency == BlockFrequency::Normal)
            readsWithTwoPaths.append(*read);
    }

    for (ReadRewrite& rewrite : rewrites)
        replaceKeepingType(context, rewrite.original, WTF::move(rewrite.rewritten));
    if (!rewrites.isEmpty())
        context.analyses.didChangeValues();

    if (!readsWithTwoPaths.isEmpty()) {
        IRBuilder builder(context.analyses.context(), context.function, context.types, &context.analyses);
        SiteExpansion expansion(context, builder);
        // An expansion moves what follows its site into another block.
        for (const CodeUnitRead& read : readsWithTwoPaths | std::views::reverse)
            expandIntoTwoPaths(expansion, read);
    }
    return static_cast<unsigned>(rewrites.size() + readsWithTwoPaths.size());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
