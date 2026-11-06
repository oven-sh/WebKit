/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSimplifyBuiltinCallsInternal.h"

#if ENABLE(AOT)

#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include "AOTRuleSupport.h"
#include "AOTSplitUnions.h"
#include "AOTStringBuilderAnalysis.h"
#include "AOTStringPositions.h"
#include "AOTStringRanges.h"
#include "AOTVector.h"
#include "PureNaN.h"
#include <ranges>
#include <span>

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

struct BlockWithCall {
    BlockID block;
    ValueID position;
};

} // anonymous namespace

static constexpr int64_t surrogateKindMask = 0xFC00;
static constexpr int64_t leadSurrogateKind = 0xD800;
static constexpr int64_t maximumCodeUnit = 0xFFFF;
static constexpr int64_t maximumCodePoint = 0x10FFFF;

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
    const Type& position = program.typeOf(context.function, arguments[0]);
    if (!position.isNumber() || !(position.primitives & Primitive::Integer))
        return std::nullopt;
    return CodeUnitRead { .site = site, .string = string, .position = arguments[0], .readsCodePoint = builtin == Builtins::StringPrototypeCodePointAt };
}

static void expandIntoTwoPaths(SiteExpansion& expansion, const CodeUnitRead& read)
{
    IRBuilder& builder = expansion.builder();
    expansion.begin(read.site);
    Expected<BlockFrequency> frequency = expansion.frequency();
    Expected<BlockFrequency> frequencyOutside = rarerOf(frequency, BlockFrequency::Unlikely);

    auto typeOf = [&](ValueID id) -> const Type& {
        return expansion.context().program.typeOf(expansion.function(), id);
    };
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

    auto finishArmOutside = [&] {
        expansion.finishArm(read.readsCodePoint ? builder.appendUndefined() : builder.appendDouble(PNaN));
    };
    for (BlockID block : blocksOutside) {
        builder.moveToEndOf(block);
        finishArmOutside();
    }
    // The call answers as the arms above do. An integer joins them as one, and the answer for outside goes on as the constant it is, so that branches behind the read can be decided for it.
    Type typeInside = Type::integer({ 0, read.readsCodePoint ? maximumCodePoint : maximumCodeUnit });
    for (const BlockWithCall& withCall : blocksWithCall) {
        builder.moveToEndOf(withCall.block);
        ValueID result = expansion.appendOriginal({ .operandIndex = OperandIndex::CallBuiltin::arguments, .operand = withCall.position, .isNarrowedValue = true });
        ValueID isInside = builder.appendTypeTest(result, typeInside);
        IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(isInside, OperandIndex::TypeTest::value, frequencyOutside, frequencyOutside);
        builder.moveToEndOf(branch.taken);
        expansion.finishArm(branch.provenOnTaken);
        builder.moveToEndOf(branch.notTaken);
        finishArmOutside();
    }
    expansion.end();
}

static bool isOnlyConcatenated(FunctionPassContext& context, ValueID string)
{
    return std::ranges::all_of(context.analyses.useLists().usesOf(string), [&](const Use& use) {
        return use.kind == Use::Kind::Operand && concatenationOpcodes.contains(context.function.at(use.user).opcode);
    });
}

static ValueID findOnlyComparison(FunctionPassContext& context, ValueID string)
{
    const Function& function = context.function;
    const UseLists& useLists = context.analyses.useLists();
    std::optional<Use> use = useLists.singleUse(string);
    if (!use || use->kind != Use::Kind::Operand || !OpcodeSet { Opcode::StrictEq, Opcode::Eq }.contains(function.at(use->user).opcode))
        return { };
    // A branch hands on what the comparison proves of its operands, and they are strings there.
    bool handsOnProofs = std::ranges::any_of(useLists.usesOf(use->user), [&](const Use& useOfComparison) {
        return useOfComparison.kind == Use::Kind::TerminatorOperand && std::ranges::any_of(function.at(useOfComparison.block).terminator.targets, [](const Target& target) {
            return !target.proofs.isEmpty();
        });
    });
    return handsOnProofs ? ValueID() : use->user;
}

// A string keeps its length, so a position below it stays inside, and `s[i]` is a string of one unit there.
// That makes `s[i] + n` a concatenation, and `s[i] === t[j]` a comparison of two code units. Selection compares a read by key with a literal.
unsigned simplifyCharacterReads(FunctionPassContext& context)
{
    Function& function = context.function;
    std::optional<StringPositions> positions;
    Vector<ValueSite, 4> joinedReads;
    Vector<std::pair<ValueID, ValueID>, 4> comparedReads;
    HashMap<ValueID, unsigned> numberOfReadsInside;
    for (const ValueSite& site : context.sitesIn(SiteList::ElementReads)) {
        const Value& read = function.at(site.value);
        if (!context.program.typeOf(function, read.objectOperand()).isAnyString())
            continue;
        bool isJoined = isOnlyConcatenated(context, site.value);
        ValueID comparison = isJoined ? ValueID() : findOnlyComparison(context, site.value);
        if ((!isJoined && !comparison) || !stringRangeSourceOf(context.program, function, read))
            continue;
        if (!positions)
            positions.emplace(context.analyses);
        if (!positions->isInside(read.objectOperand(), read.propertyOperand(), site.block))
            continue;
        if (isJoined)
            joinedReads.append(site);
        else {
            comparedReads.append({ comparison, site.value });
            ++numberOfReadsInside.add(comparison, 0).iterator->value;
        }
    }
    comparedReads.removeAllMatching([&](const auto& read) {
        return numberOfReadsInside.get(read.first) != 2;
    });
    if (joinedReads.isEmpty() && comparedReads.isEmpty())
        return 0;

    IRBuilder builder(context.analyses.context(), function, context.types, &context.analyses);
    Vector<ValueID, 4> ends;
    for (const ValueSite& site : joinedReads) {
        builder.setOrigin(function.at(site.value).origin());
        ValueID one = builder.insertBefore(site.point(), makeInteger(1));
        ValueID end = builder.insertBefore(site.point(), IRBuilder::make(Opcode::Add, { function.at(site.value).propertyOperand(), one }));
        // The end is at most the length, also where the type of the position is too wide to say so.
        if (!context.program.typeOf(function, end).isInteger())
            end = builder.insertBefore(site.point(), IRBuilder::make(Opcode::ToInt32, { end }));
        ends.append(end);
    }
    builder.applyInsertions();

    for (size_t index = 0; index < joinedReads.size(); ++index) {
        const Value& read = function.at(joinedReads[index].value);
        replaceKeepingType(context, joinedReads[index].value, IRBuilder::make(Opcode::Substring, { read.objectOperand(), read.propertyOperand(), ends[index] }));
    }
    for (auto [comparison, id] : comparedReads) {
        const Value& read = function.at(id);
        replaceKeepingType(context, id, IRBuilder::make(Opcode::GetStringCodeUnit, { read.objectOperand(), read.propertyOperand() }));
    }
    context.analyses.didChangeValues();
    return static_cast<unsigned>(joinedReads.size() + comparedReads.size());
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
        if (read->isInside && !read->readsCodePoint)
            rewrite(site.value, IRBuilder::make(Opcode::GetStringCodeUnit, { read->string, read->position }));
        else if (context.function.at(site.block).frequency.predicted() == BlockFrequency::Normal)
            readsWithTwoPaths.append(*read);
    }

    for (ReadRewrite& rewrite : rewrites)
        replaceKeepingType(context, rewrite.original, WTF::move(rewrite.rewritten));
    if (!rewrites.isEmpty())
        context.analyses.didChangeValues();

    if (!readsWithTwoPaths.isEmpty()) {
        IRBuilder builder(context.analyses.context(), context.function, context.types, &context.analyses);
        SiteExpansion expansion(context, builder);
        for (const CodeUnitRead& read : readsWithTwoPaths | std::views::reverse)
            expandIntoTwoPaths(expansion, read);
    }
    return static_cast<unsigned>(rewrites.size() + readsWithTwoPaths.size());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
