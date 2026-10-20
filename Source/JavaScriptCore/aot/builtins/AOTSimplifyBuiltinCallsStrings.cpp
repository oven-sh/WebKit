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
};

} // anonymous namespace

static std::optional<CodeUnitRead> codeUnitReadAtIntegerOf(FunctionPassContext& context, const ValueSite& site, const Value& call)
{
    const Program& program = context.program;
    if (program.builtins().at(call.overload()).builtin != Builtins::StringPrototypeCharCodeAt)
        return std::nullopt;

    ValueID string = call.receiverOperand();
    std::span<const ValueID> arguments = call.argumentsOperands();
    if (!string || arguments.size() != 1 || !program.typeOf(context.function, string).isAnyString() || !program.typeOf(context.function, arguments[0]).isInteger())
        return std::nullopt;
    // Selection reads the characters of a range in the string it was cut from, and knows the call for that.
    if (stringRangeSourceOf(program, context.function, context.function.at(context.analyses.sameObjectThroughCasts(string))))
        return std::nullopt;
    return CodeUnitRead { site, string, arguments[0] };
}

// Inside the string the answer is an integer, outside it is NaN. With the test in the IR the operations behind the read that want an integer get one:
// PlaceTypeTests tests the joined value for them, and the edge from each side skips that test.
static void expandIntoTwoPaths(SiteExpansion& expansion, const CodeUnitRead& read)
{
    IRBuilder& builder = expansion.builder();
    expansion.begin(read.site);
    BlockFrequency frequency = expansion.frequency();
    BlockFrequency frequencyOutside = rarerOf(frequency, BlockFrequency::Unlikely);

    // A position that lies outside every string has the type never on the path inside, and nothing is computed from a value that cannot exist.
    auto mayBeInside = [&](ValueID position) {
        return expansion.function().at(position).type != TypeTable::never;
    };

    Vector<BlockID, 2> blocksOutside;
    ValueID position = read.position;
    if (expansion.context().program.typeOf(expansion.function(), position).integers.minimum < 0) {
        ValueID isNotNegative = expansion.append(IRBuilder::make(Opcode::LessEq, { builder.appendInteger(0), position }));
        IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(isNotNegative, OperandIndex::LessEq::right, frequency, frequencyOutside);
        blocksOutside.append(branch.notTaken);
        position = branch.provenOnTaken;
    }
    if (mayBeInside(position)) {
        ValueID length = expansion.append(IRBuilder::make(Opcode::GetStringLength, { read.string }));
        ValueID isBelowLength = expansion.append(IRBuilder::make(Opcode::Less, { position, length }));
        IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(isBelowLength, OperandIndex::Less::left, frequency, frequencyOutside);
        blocksOutside.append(branch.notTaken);
        position = branch.provenOnTaken;
    }
    if (mayBeInside(position))
        expansion.finishArm(expansion.append(IRBuilder::make(Opcode::GetStringCodeUnit, { read.string, position })));
    else
        expansion.finishArmAsUnreachable();

    for (BlockID block : blocksOutside) {
        builder.moveToEndOf(block);
        expansion.finishArm(builder.appendDouble(PNaN));
    }
    expansion.end();
}

unsigned simplifyStringReads(FunctionPassContext& context)
{
    StringPositions positions(context.analyses);
    Vector<ReadRewrite> rewrites;
    Vector<CodeUnitRead> readsWithTwoPaths;
    auto rewrite = [&](ValueID original, Value&& rewritten) {
        if (keepsTypeOf(context, original, rewritten))
            rewrites.append(ReadRewrite { original, WTF::move(rewritten) });
    };
    forEachValueSite(context.analyses, context.function, [&](const ValueSite& site) {
        const Value& call = context.function.at(site.value);
        if (call.opcode != Opcode::CallBuiltin || call.builtinCallForm() != BuiltinCallForm::Whole)
            return;
        if (ValueID string = stringMeasuredBy(context.program, context.function, call)) {
            rewrite(site.value, IRBuilder::make(Opcode::GetStringLength, { string }));
            return;
        }
        std::optional<CodeUnitRead> read = codeUnitReadAtIntegerOf(context, site, call);
        if (!read)
            return;
        if (positions.isInside(read->string, read->position, site.block))
            rewrite(site.value, IRBuilder::make(Opcode::GetStringCodeUnit, { read->string, read->position }));
        else if (context.function.at(site.block).frequency == BlockFrequency::Normal)
            readsWithTwoPaths.append(*read);
    });

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
