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
#include <algorithm>
#include <limits>
#include <optional>
#include <wtf/BitVector.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct UseOfRange {
    ValueID used;
    ValueID user;
    uint32_t operandIndex { 0 };
    ProgramPoint point;
};

// `view.subarray(begin)` and every value that may be what it answers.
struct RangeOfBytes {
    ValueSite call;
    Vector<ValueID, 4> parameters;
    Vector<ValueLocation, 4> locationsOfParameters;
    Vector<ValueID, 4> casts;
    Vector<UseOfRange, 2> encodings;
    Vector<UseOfRange, 4> otherUses;
    Vector<BlockID, 8> blocksThatHoldIt;
};

} // anonymous namespace

static constexpr size_t maxValuesHoldingRange = 8;
static constexpr size_t maxUsesOfRange = 8;

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

// The overload for a receiver that gives a view of its own class whatever the program has done: it runs no code of the program.
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

static void addOnce(auto& list, auto element)
{
    if (!list.contains(element))
        list.append(element);
}

static std::optional<RangeOfBytes> findValuesAndUses(FunctionPassContext& context, const ValueSite& site)
{
    const Function& function = context.function;
    const UseLists& useLists = context.analyses.useLists();
    RangeOfBytes range { site, { }, { }, { }, { }, { }, { } };
    Vector<ValueID, 8> worklist { site.value };
    while (!worklist.isEmpty()) {
        ValueID current = worklist.takeLast();
        for (const Use& use : useLists.usesOf(current)) {
            addOnce(range.blocksThatHoldIt, use.block);
            if (use.kind == Use::Kind::EdgeArgument) {
                BlockID target = targetOf(function, use.edge()).block;
                ValueID parameter = function.at(target).parameters[use.operandIndex];
                if (range.parameters.contains(parameter))
                    continue;
                range.parameters.append(parameter);
                range.locationsOfParameters.append(useLists.locationOf(parameter));
                addOnce(range.blocksThatHoldIt, target);
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

// Whoever sees the range gets one made for him, so nobody may see it twice: behind a use, no way leads to anything that holds the range but through the call, which makes the next one.
static bool isSeenOnceOnEveryWay(FunctionPassContext& context, const RangeOfBytes& range)
{
    const CFG& cfg = context.analyses.cfg();
    auto leadsBackToRange = [&](BlockID from) {
        BitVector seen;
        Vector<BlockID, 16> worklist;
        auto visit = [&](BlockID block) {
            if (!seen.set(block.index()))
                worklist.append(block);
        };
        for (BlockID successor : cfg.successors(from))
            visit(successor);
        while (!worklist.isEmpty()) {
            BlockID block = worklist.takeLast();
            if (block == range.call.block)
                continue;
            if (range.blocksThatHoldIt.contains(block))
                return true;
            for (BlockID successor : cfg.successors(block))
                visit(successor);
        }
        return false;
    };

    Vector<BlockID, 8> blocksOfUses;
    auto isAloneInItsBlock = [&](const UseOfRange& use) {
        // The call's own block holds the range behind the call, and a way back into the block makes the next one.
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

    // A block that uses it hands it to nobody.
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

static std::optional<Value> makeCallOfHelper(FunctionPassContext& context, BuiltinID helper, const BuiltinCallStamps& stamps, std::initializer_list<ValueID> arguments)
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

// `target = view.subarray(written); counts = encoder.encodeInto(text, target)`: a view is made for every piece of text that is written, and is dead behind the call.
// In its place travel the view it is a range of and the place where the range starts, -1 where the value is what it is. See %CompilerHelpers% in the table of built-ins.
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
        std::optional<Value> startOfRange = makeCallOfHelper(context, Builtins::CompilerHelpersStartOfRangeOfBytes, { }, { view, begin });
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

        std::optional<Value> viewOrRange = makeCallOfHelper(context, Builtins::CompilerHelpersViewOrRangeOfBytes, stampsOf(call), { view, begin, start });
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
            std::optional<Value> encodingIntoRange = makeCallOfHelper(context, Builtins::CompilerHelpersEncodeIntoRangeOfBytes, stampsOf(encoding), { encoding.receiverOperand(), encoding.argumentsOperands()[0], use.used, startOf(use.used) });
            RELEASE_ASSERT(encodingIntoRange);
            replace(use.user, encoding, WTF::move(*encodingIntoRange));
        }
        for (const UseOfRange& use : range.otherUses) {
            builder.setOrigin(function.at(use.user).origin());
            std::optional<Value> made = makeCallOfHelper(context, Builtins::CompilerHelpersRangeOfBytes, { }, { use.used, startOf(use.used) });
            RELEASE_ASSERT(made);
            function.edit(use.user).setOperand(use.operandIndex, builder.insertBefore(use.point, WTF::move(*made)));
        }
        ++rewrites;
    }
    builder.applyInsertions();
    context.analyses.didChangeValues();
    return rewrites;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
