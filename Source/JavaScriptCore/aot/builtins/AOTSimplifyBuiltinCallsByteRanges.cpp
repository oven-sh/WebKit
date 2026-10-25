/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSimplifyBuiltinCallsInternal.h"

#if ENABLE(AOT)

#include "AOTBuiltinCallPlan.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include "AOTSplitUnions.h"
#include "AOTThreshold.h"
#include "AOTVector.h"
#include <algorithm>
#include <limits>
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct UseOfRange {
    ValueID used;
    ValueID user;
    uint32_t operandIndex { 0 };
    ProgramPoint point;
};

struct RangeOfBytes {
    ValueSite call;
    Vector<ValueID, 4> parameters;
    Vector<ValueLocation, 4> locationsOfParameters;
    Vector<ValueID, 4> casts;
    Vector<UseOfRange, 2> encodings;
    Vector<UseOfRange, 4> otherUses;
};

} // anonymous namespace

AOT_THRESHOLD(size_t, maxValuesHoldingRange, 8);
AOT_THRESHOLD(size_t, maxUsesOfRange, 8);

static const BuiltinOverload* findCallOf(const Program& program, const Value& value, BuiltinID function, size_t numberOfArguments)
{
    if (value.opcode != Opcode::CallBuiltin || !value.receiverOperand() || value.argumentsOperands().size() != numberOfArguments)
        return nullptr;
    const BuiltinOverload& overload = program.builtins().at(value.overload());
    if (overload.mode != BuiltinCallMode::Call || program.builtins().canonicalID(overload.builtin) != function)
        return nullptr;
    return &overload;
}

static bool isInstanceOf(FunctionPassContext& context, ValueID value, BuiltinClassID builtinClass)
{
    return context.program.typeOf(context.function, value).isSubsetOf(Type::builtinClass(builtinClass), context.program);
}

static bool makesRangeOfBytes(FunctionPassContext& context, const Value& value)
{
    const Program& program = context.program;
    if (!findCallOf(program, value, Builtins::TypedArrayPrototypeSubarray, 1) || value.builtinCallForm() != BuiltinCallForm::Whole)
        return false;
    return value.overload() == program.builtins().overloadIDsOf(Builtins::TypedArrayPrototypeSubarray).at(0) && isInstanceOf(context, value.receiverOperand(), BuiltinClasses::Uint8Array);
}

static bool isDestinationOfEncoding(FunctionPassContext& context, const Value& user, ValueID used)
{
    if (!findCallOf(context.program, user, Builtins::TextEncoderPrototypeEncodeInto, 2))
        return false;
    if (user.builtinCallForm() != BuiltinCallForm::Whole && user.builtinCallForm() != BuiltinCallForm::AnyReceiver)
        return false;
    std::span<const ValueID> arguments = user.argumentsOperands();
    return arguments[1] == used && arguments[0] != used && user.receiverOperand() != used && isInstanceOf(context, user.receiverOperand(), BuiltinClasses::TextEncoder);
}

static std::optional<RangeOfBytes> findValuesAndUses(FunctionPassContext& context, const ValueSite& site)
{
    const Function& function = context.function;
    const UseLists& useLists = context.analyses.useLists();
    RangeOfBytes range { site, { }, { }, { }, { }, { } };
    Vector<ValueID, 8> worklist { site.value };
    while (!worklist.isEmpty()) {
        ValueID current = worklist.takeLast();
        for (const Use& use : useLists.usesOf(current)) {
            if (use.kind == Use::Kind::EdgeArgument) {
                BlockID target = targetOf(function, use.edge()).block;
                ValueID parameter = function.at(target).parameters[use.operandIndex];
                if (range.parameters.contains(parameter))
                    continue;
                range.parameters.append(parameter);
                range.locationsOfParameters.append(useLists.locationOf(parameter));
                worklist.append(parameter);
                continue;
            }
            if (use.kind != Use::Kind::Operand)
                return std::nullopt;
            const Value& user = function.at(use.user);
            if (isCastWithoutTest(user.opcode) && !use.operandIndex) {
                range.casts.append(use.user);
                worklist.append(use.user);
                continue;
            }
            UseOfRange found { current, use.user, use.operandIndex, useLists.pointOf(use) };
            if (isDestinationOfEncoding(context, user, current))
                range.encodings.append(found);
            else
                range.otherUses.append(found);
        }
        if (range.parameters.size() + range.casts.size() > maxValuesHoldingRange || range.encodings.size() + range.otherUses.size() > maxUsesOfRange)
            return std::nullopt;
    }
    if (range.encodings.isEmpty())
        return std::nullopt;
    return range;
}

// Follows the range that a use in the block has seen, and answers whether another use sees the same range later.
// A value keeps the range while control stays below its definition. A block parameter gets it from an edge argument. So a loop that makes a new range on each turn does not see the old one again.
static bool isSeenAgainBehind(FunctionPassContext& context, const RangeOfBytes& range, BlockID from)
{
    const Function& function = context.function;
    const UseLists& useLists = context.analyses.useLists();
    const Dominators& dominators = context.analyses.dominators();

    using Holders = uint32_t;
    Vector<ValueID, 16> holders { range.call.value };
    holders.appendVector(range.parameters);
    holders.appendVector(range.casts);
    static_assert(1 + maxValuesHoldingRange < 32);
    auto bitOf = [&](ValueID value) -> Holders {
        size_t index = holders.find(value);
        return index == notFound ? 0 : Holders { 1 } << index;
    };

    auto holdersBehind = [&](Holders held, const Target& target) {
        Holders next = 0;
        for (size_t index = 0; index < holders.size(); ++index) {
            if ((held >> index & 1) && dominators.strictlyDominates(useLists.blockOf(holders[index]), target.block))
                next |= Holders { 1 } << index;
        }
        const Block& block = function.at(target.block);
        for (size_t index = 0; index < target.arguments.size(); ++index) {
            if (held & bitOf(target.arguments[index]))
                next |= bitOf(block.parameters[index]);
        }
        // A cast is listed behind the value that it casts.
        for (ValueID cast : range.casts) {
            if (useLists.blockOf(cast) == target.block && (next & bitOf(function.at(cast).operand(0))))
                next |= bitOf(cast);
        }
        return next;
    };
    auto isUsedIn = [&](BlockID block, Holders held) {
        auto seesIt = [&](const UseOfRange& use) {
            return use.point.block == block && (held & bitOf(use.used));
        };
        return std::ranges::any_of(range.encodings, seesIt) || std::ranges::any_of(range.otherUses, seesIt);
    };

    Holders heldAtUse = 0;
    for (size_t index = 0; index < holders.size(); ++index) {
        if (dominators.dominates(useLists.blockOf(holders[index]), from))
            heldAtUse |= Holders { 1 } << index;
    }
    Vector<Holders> heldAt;
    heldAt.fill(0, function.blocks.size());
    Vector<std::pair<BlockID, Holders>, 16> worklist { { from, heldAtUse } };
    while (!worklist.isEmpty()) {
        auto [block, held] = worklist.takeLast();
        for (uint32_t index = 0; index < numberOfOutgoingEdges(function.at(block)); ++index) {
            const Target& target = targetOf(function, outgoingEdgeAt(function, block, index));
            Holders& known = heldAt[target.block.index()];
            Holders added = holdersBehind(held, target) & ~known;
            if (!added)
                continue;
            known |= added;
            if (isUsedIn(target.block, added))
                return true;
            worklist.append({ target.block, added });
        }
    }
    return false;
}

static bool isSeenOnceOnEveryWay(FunctionPassContext& context, const RangeOfBytes& range)
{
    auto leadsBackToRange = [&](BlockID from) {
        return isSeenAgainBehind(context, range, from);
    };

    Vector<BlockID, 8> blocksOfUses;
    auto isAloneInItsBlock = [&](const UseOfRange& use) {
        if (blocksOfUses.contains(use.point.block))
            return false;
        blocksOfUses.append(use.point.block);
        return true;
    };
    auto isLastToSeeIt = [&](const UseOfRange& use) {
        return isAloneInItsBlock(use) && !leadsBackToRange(use.point.block);
    };
    if (!std::ranges::all_of(range.encodings, isLastToSeeIt) || !std::ranges::all_of(range.otherUses, isLastToSeeIt))
        return false;

    const Function& function = context.function;
    auto holdsRange = [&](ValueID value) {
        return value == range.call.value || range.parameters.contains(value) || range.casts.contains(value);
    };
    for (BlockID block : blocksOfUses) {
        bool handsItOn = false;
        function.at(block).forEachTarget([&](const Target& target) {
            handsItOn |= std::ranges::any_of(target.arguments, holdsRange);
        });
        if (handsItOn)
            return false;
    }
    return true;
}

static bool canTakeAnotherParameter(FunctionPassContext& context, const RangeOfBytes& range)
{
    return std::ranges::all_of(range.locationsOfParameters, [&](const ValueLocation& location) {
        if (location.block == entryBlock || context.function.at(location.block).isHandlerBlock())
            return false;
        return std::ranges::none_of(context.analyses.cfg().incomingEdges(location.block), &Edge::isHandlerEdge);
    });
}

static std::optional<Value> makeStampedCallOfHelper(FunctionPassContext& context, BuiltinID helper, const BuiltinCallStamps& stamps, std::initializer_list<ValueID> arguments)
{
    TypingContext typing { context.program, context.function, context.types };
    for (BuiltinOverloadID overload : context.program.builtins().overloadIDsOf(helper)) {
        Value call = makeBuiltinCall(overload, BuiltinCallForm::Whole, { { }, arguments }, stamps);
        if (overloadTakesOperands(typing, call))
            return call;
    }
    return std::nullopt;
}

static BuiltinCallStamps stampsOf(const Value& call)
{
    return { call.expectedResult(), call.accessSite(), call.allocationSite() };
}

unsigned leaveRangesOfBytesUnmade(FunctionPassContext& context, const BuiltinCallSites& calls)
{
    Function& function = context.function;
    Vector<RangeOfBytes, 2> ranges;
    auto isHeldByAnother = [&](const RangeOfBytes& candidate) {
        return std::ranges::any_of(ranges, [&](const RangeOfBytes& taken) {
            return std::ranges::any_of(candidate.parameters, [&](ValueID parameter) {
                return taken.parameters.contains(parameter);
            });
        });
    };
    for (const ValueSite& site : calls) {
        if (!makesRangeOfBytes(context, function.at(site.value)))
            continue;
        std::optional<RangeOfBytes> range = findValuesAndUses(context, site);
        if (!range || !canTakeAnotherParameter(context, *range) || !isSeenOnceOnEveryWay(context, *range) || isHeldByAnother(*range))
            continue;
        ranges.append(WTF::move(*range));
    }
    if (ranges.isEmpty())
        return 0;

    TypeID typeOfStart = context.types.intern(Type::integer(Range { -1, std::numeric_limits<int32_t>::max() }));
    IRBuilder builder(context.analyses.context(), function, context.types, &context.analyses);
    unsigned rewrites = 0;
    for (const RangeOfBytes& range : ranges) {
        const Value call = function.at(range.call.value);
        ValueID view = call.receiverOperand();
        ValueID begin = call.argumentsOperands()[0];
        std::optional<Value> startOfRange = makeStampedCallOfHelper(context, Builtins::CompilerHelpersStartOfRangeOfBytes, { }, { view, begin });
        if (!startOfRange)
            continue;

        builder.setOrigin(call.origin());
        Value noStart = IRBuilder::make(Opcode::Integer);
        noStart.setInteger(-1);
        ValueID isWhatItIs = builder.insertBefore(ProgramPoint::atHeadOf(entryBlock), WTF::move(noStart));
        ValueID start = builder.insertBefore(range.call.point(), WTF::move(*startOfRange));

        Vector<ValueID, 4> startsOfParameters;
        for (const ValueLocation& location : range.locationsOfParameters)
            startsOfParameters.append(builder.addParameter(location.block, typeOfStart));
        auto startOf = [&](ValueID value) {
            while (range.casts.contains(value))
                value = function.at(value).operand(0);
            if (value == range.call.value)
                return start;
            size_t index = range.parameters.find(value);
            return index == notFound ? isWhatItIs : startsOfParameters[index];
        };
        for (const ValueLocation& location : range.locationsOfParameters) {
            for (Edge edge : context.analyses.cfg().incomingEdges(location.block))
                builder.appendArgument(edge, startOf(targetOf(function, edge).arguments[location.index]));
        }

        std::optional<Value> viewOrRange = makeStampedCallOfHelper(context, Builtins::CompilerHelpersViewOrRangeOfBytes, stampsOf(call), { view, begin, start });
        RELEASE_ASSERT(viewOrRange);
        auto replace = [&](ValueID id, const Value& replaced, Value&& replacement) {
            replacement.setOrigin(replaced.origin());
            replacement.setIsDemotedTailCall(replaced.isDemotedTailCall());
            replacement.type = replaced.type;
            replacement.setExpected(replaced.expected());
            function.edit(id) = WTF::move(replacement);
        };
        replace(range.call.value, call, WTF::move(*viewOrRange));

        for (const UseOfRange& use : range.encodings) {
            const Value encoding = function.at(use.user);
            std::optional<Value> encodingIntoRange = makeStampedCallOfHelper(context, Builtins::CompilerHelpersEncodeIntoRangeOfBytes, stampsOf(encoding), { encoding.receiverOperand(), encoding.argumentsOperands()[0], use.used, startOf(use.used) });
            RELEASE_ASSERT(encodingIntoRange);
            replace(use.user, encoding, WTF::move(*encodingIntoRange));
        }
        for (const UseOfRange& use : range.otherUses) {
            builder.setOrigin(function.at(use.user).origin());
            std::optional<Value> made = makeStampedCallOfHelper(context, Builtins::CompilerHelpersRangeOfBytes, { }, { use.used, startOf(use.used) });
            RELEASE_ASSERT(made);
            ValueID rangeForUse = builder.insertBefore(use.point, WTF::move(*made));
            function.edit(use.user, ValueEdit::Definition).setOperand(use.operandIndex, rangeForUse);
        }
        ++rewrites;
    }
    builder.applyInsertions();
    context.analyses.didChangeValues();
    return rewrites;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
