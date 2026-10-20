/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTExpandIteration.h"

#if ENABLE(AOT)

#include "AOTBuiltinMembers.h"
#include "AOTBuiltinPristine.h"
#include "AOTBuiltinTable.h"
#include "AOTFunctionAnalyses.h"
#include "AOTSplitStringWalk.h"
#include "AOTTypeTestSite.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

namespace {

using Arms = decltype(IterationPlan::arms);

struct PlannedValue {
    ValueID value;
    BlockID block;
    OptionSet<PristineAspects> aspects;
    IteratorKind kind { IteratorKind::Sync };
    bool isSentValues { false };
};

} // anonymous namespace

static std::optional<IterationForm> formOfClass(const BuiltinTable& builtins, BuiltinClassID id, IteratorKind kind)
{
    switch (kind) {
    case IteratorKind::Sync:
        break;
    case IteratorKind::Async:
        if (id == BuiltinClasses::AsyncGenerator)
            return IterationForm::AsyncGenerator;
        return std::nullopt;
    }

    if (id == BuiltinClasses::Array)
        return IterationForm::Array;
    if (id == BuiltinClasses::Map)
        return IterationForm::Map;
    if (id == BuiltinClasses::Set)
        return IterationForm::Set;
    if (id == BuiltinClasses::Generator)
        return IterationForm::Generator;
    if (isTypedView(builtins.at(id).typedArrayType))
        return IterationForm::TypedArray;
    return std::nullopt;
}

static IterationArm armForPart(FunctionPassContext& pass, const ClassPart& part, IterationForm form)
{
    return IterationArm {
        .form = form,
        .kind = form == IterationForm::Map ? JSC::IterationKind::Entries : JSC::IterationKind::Values,
        .builtinClass = part.builtinClass,
        .typedArrayType = pass.program.builtins().at(part.builtinClass).typedArrayType,
        .testedType = pass.types.intern(Type::builtinClass(part.builtinClass, part.contents.span())),
    };
}

// Flow proves an instance pristine from what is done to the instance. What its prototypes hold under @@iterator has a proof of its own.
static bool hasConstantIteratorMethod(FunctionPassContext& pass, const IterationArm& arm)
{
    const Program& program = pass.program;
    BuiltinClassID receiverClass = arm.form == IterationForm::String ? BuiltinClasses::String : arm.builtinClass;
    WellKnownSymbol symbol = arm.form == IterationForm::AsyncGenerator ? WellKnownSymbol::AsyncIterator : WellKnownSymbol::Iterator;
    PropertyKeyID key = program.keyOf(program.builtins().nameOf(symbol));
    return findMemberOfInstances({ program, pass.inputs.flow }, receiverClass, key).isConstant();
}

static bool hasFailedEarlier(FunctionPassContext& pass, Value&& test, BlockID block)
{
    return pass.analyses.findEarlierTest(test, block) == EarlierTest::Failed;
}

static bool hasKnownStringPart(const Type& type)
{
    return type.primitives & Primitive::AnyString && !type.withPrimitives(Primitive::UndefinedOrNull).isAnything();
}

static void appendCandidate(Arms& candidates, IterationArm&& arm)
{
    bool isListed = candidates.containsIf([&](const IterationArm& other) {
        return other.form == arm.form && other.builtinClass == arm.builtinClass;
    });
    if (!isListed)
        candidates.append(WTF::move(arm));
}

static void appendCandidatesOf(FunctionPassContext& pass, const Type& type, IteratorKind kind, Arms& candidates)
{
    const BuiltinTable& builtins = pass.program.builtins();
    for (const ClassPart& part : type.classes) {
        if (std::optional<IterationForm> form = formOfClass(builtins, part.builtinClass, kind))
            appendCandidate(candidates, armForPart(pass, part, *form));
    }
    if (kind == IteratorKind::Sync && hasKnownStringPart(type))
        appendCandidate(candidates, IterationArm { .form = IterationForm::String, .testedType = TypeTable::string });
}

// What an iterable that nobody knows anything about is tested for. Of the classes with an arm only a generator does anything with what it is sent.
static Vector<BuiltinClassID, 2> classesExpectedOfUnknownIterable(const PlannedValue& planned)
{
    switch (planned.kind) {
    case IteratorKind::Sync:
        if (planned.isSentValues)
            return { BuiltinClasses::Generator, BuiltinClasses::Array };
        return { BuiltinClasses::Array };
    case IteratorKind::Async:
        return { BuiltinClasses::AsyncGenerator };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Arms collectCandidates(FunctionPassContext& pass, const PlannedValue& planned)
{
    const Type& predicted = pass.program.predictionOrTypeOf(pass.function, planned.value);

    Arms candidates;
    appendCandidatesOf(pass, predicted, planned.kind, candidates);
    appendCandidatesOf(pass, pass.program.typeOf(pass.function, planned.value), planned.kind, candidates);
    if (!predicted.anyObject)
        return candidates;

    for (BuiltinClassID expectedClass : classesExpectedOfUnknownIterable(planned)) {
        ClassPart expected;
        expected.builtinClass = expectedClass;
        appendCandidate(candidates, armForPart(pass, expected, *formOfClass(pass.program.builtins(), expectedClass, planned.kind)));
    }
    return candidates;
}

static bool areNamesProven(FunctionPassContext& pass, const PlannedValue& planned, const IterationArm& arm)
{
    if (arm.form == IterationForm::String)
        return true;
    Type instances = pass.program.typeOf(pass.function, planned.value).intersectWith(pass.types.at(arm.testedType), pass.types);
    return pristineAspectsProvenBy(instances, arm.builtinClass).containsAll(planned.aspects);
}

static bool canTestNames(const BuiltinTable& builtins, const IterationArm& arm)
{
    switch (arm.form) {
    case IterationForm::Array:
    case IterationForm::TypedArray:
    case IterationForm::Map:
    case IterationForm::Set:
        return pristineTestOf(builtins, arm.builtinClass) == PristineTest::MayPass;
    case IterationForm::Generator:
    case IterationForm::AsyncGenerator:
        return true;
    case IterationForm::String:
    case IterationForm::SplitString:
    case IterationForm::Protocol:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Value makeNamesTest(const PlannedValue& planned, const IterationArm& arm)
{
    if (arm.form == IterationForm::Generator)
        return IRBuilder::make(Opcode::IsUnmodifiedGenerator, { planned.value });
    if (arm.form == IterationForm::AsyncGenerator)
        return IRBuilder::make(Opcode::IsUnmodifiedAsyncGenerator, { planned.value });
    return makePristineTest(planned.value, arm.builtinClass, planned.aspects);
}

static bool canServe(FunctionPassContext& pass, const PlannedValue& planned, const IterationArm& arm)
{
    const Program& program = pass.program;
    if (!program.typeOf(pass.function, planned.value).overlaps(pass.types.at(arm.testedType), program))
        return false;
    if (!hasConstantIteratorMethod(pass, arm))
        return false;
    if (!arm.namesAreProven && !canTestNames(program.builtins(), arm))
        return false;
    if (hasFailedEarlier(pass, makeTypeTest(planned.value, arm.testedType), planned.block))
        return false;
    return arm.namesAreProven || !hasFailedEarlier(pass, makeNamesTest(planned, arm), planned.block);
}

static Arms findClassArms(FunctionPassContext& pass, const PlannedValue& planned)
{
    Arms arms;
    for (IterationArm& arm : collectCandidates(pass, planned)) {
        arm.namesAreProven = areNamesProven(pass, planned, arm);
        if (canServe(pass, planned, arm))
            arms.append(arm);
    }
    if (pass.exceeds(LimitID::MaxDispatchCases, arms.size(), planned.value))
        arms.shrink(static_cast<size_t>(pass.limit(LimitID::MaxDispatchCases)));
    return arms;
}

static IterationPlan planWithClassArms(FunctionPassContext& pass, ValueID value, Arms&& classArms)
{
    const Program& program = pass.program;
    IterationPlan plan;
    plan.source = value;

    Type remaining = program.typeOf(pass.function, value);
    for (IterationArm& arm : classArms) {
        const Type& tested = pass.types.at(arm.testedType);
        if (!remaining.overlaps(tested, program))
            continue;
        arm.classIsProven = remaining.isSubsetOf(tested, program);
        remaining = remaining.without(tested, pass.types);
        plan.arms.append(arm);
    }

    bool restNeedsProtocol = !remaining.isNever() && !remaining.isUndefinedOrNull();
    bool namesTestMayFail = !std::ranges::all_of(plan.arms, &IterationArm::namesAreProven);
    if (plan.arms.isEmpty() || restNeedsProtocol || namesTestMayFail)
        plan.arms.append(IterationArm { });
    return plan;
}

static IterationPlan planForValue(FunctionPassContext& pass, const PlannedValue& planned)
{
    return planWithClassArms(pass, planned.value, findClassArms(pass, planned));
}

static bool makesIteratorOver(InlineBuiltin maker, IterationForm form)
{
    switch (form) {
    case IterationForm::Array:
    case IterationForm::TypedArray:
        return maker == InlineBuiltin::ArrayIterator;
    case IterationForm::Map:
        return maker == InlineBuiltin::MapIterator;
    case IterationForm::Set:
        return maker == InlineBuiltin::SetIterator;
    case IterationForm::Protocol:
    case IterationForm::String:
    case IterationForm::SplitString:
    case IterationForm::Generator:
    case IterationForm::AsyncGenerator:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

namespace {

struct BypassedIterator {
    ValueID receiver;
    IterationArm arm;
};

struct MergedIterators {
    ValueID open;
    BlockID blockOfOpen;
    Arms classArms;
    Vector<ArmSource, 2> sources;
    Vector<BlockID, 2> sourcesOfOtherValues;
    Vector<BlockID, 2> mergingBlocks;
    // A test behind a merge runs whichever block control came from, so it has to find the iterator.
    bool isTestedAfterMerge { false };
};

} // anonymous namespace

static std::optional<BypassedIterator> findBypassedIterator(FunctionPassContext& pass, ValueID definition, BlockID blockOfOpen)
{
    const Program& program = pass.program;
    const Function& function = pass.function;
    const BuiltinTable& builtins = program.builtins();

    const Value& call = function.at(definition);
    if (call.opcode != Opcode::CallBuiltin || call.builtinCallForm() != BuiltinCallForm::Whole || !call.receiverOperand())
        return std::nullopt;
    const BuiltinImplementation& implementation = builtins.at(call.overload()).code.implementation;
    if (!implementation.inlineBuiltin)
        return std::nullopt;

    ValueID receiver = call.receiverOperand();
    const Type& receiverType = program.typeOf(function, receiver);
    const ClassPart* part = receiverType.isObject() ? receiverType.singleBuiltinClass() : nullptr;
    if (!part)
        return std::nullopt;
    std::optional<IterationForm> form = formOfClass(builtins, part->builtinClass, IteratorKind::Sync);
    if (!form || !makesIteratorOver(*implementation.inlineBuiltin, *form))
        return std::nullopt;
    // An open inside a loop with its call outside resumes the same iterator on every run: only the first run starts at the beginning.
    if (!pass.analyses.blockCycles().isInEveryCycleOf(pass.analyses.useLists().blockOf(definition), blockOfOpen))
        return std::nullopt;

    return BypassedIterator { receiver, IterationArm {
        .form = *form,
        .kind = static_cast<JSC::IterationKind>(implementation.inlineParameters[0]),
        .builtinClass = part->builtinClass,
        .typedArrayType = builtins.at(part->builtinClass).typedArrayType,
        .testedType = function.at(receiver).type,
        .classIsProven = true,
        .namesAreProven = true,
        .bypassesIterator = true,
    } };
}

static bool hasCastWithTest(const Function& function, std::span<const ValueID> definitionAndCasts)
{
    return std::ranges::any_of(definitionAndCasts.subspan(1), [&](ValueID cast) {
        return !isCastWithoutTest(function.at(cast).opcode);
    });
}

static bool isSourceOrMergingBlock(const MergedIterators& merged, BlockID block)
{
    return merged.mergingBlocks.contains(block) || merged.sourcesOfOtherValues.contains(block) || merged.sources.containsIf([&](const ArmSource& source) {
        return source.block == block;
    });
}

static std::optional<unsigned> findOrAddClassArm(FunctionPassContext& pass, MergedIterators& merged, const IterationArm& arm)
{
    size_t index = merged.classArms.find(arm);
    if (index != notFound)
        return static_cast<unsigned>(index);
    if (pass.exceeds(LimitID::MaxDispatchCases, merged.classArms.size() + 1, merged.open))
        return std::nullopt;
    merged.classArms.append(arm);
    return static_cast<unsigned>(merged.classArms.size() - 1);
}

static bool collectSourcesOfParameter(FunctionPassContext&, MergedIterators&, ValueID parameter);

// False where one block would have to stand for two edges.
static bool collectSourcesOfArgument(FunctionPassContext& pass, MergedIterators& merged, Edge edge, ValueID argument)
{
    const Function& function = pass.function;
    const UseLists& useLists = pass.analyses.useLists();
    if (isSourceOrMergingBlock(merged, edge.from))
        return false;

    std::optional<Use> use = useLists.singleUse(argument);
    Vector<ValueID, 2> chain;
    if (use && use->kind == Use::Kind::EdgeArgument) {
        if (isCastOrConversion(function.at(argument).opcode))
            chain = findSingleUseCastChain(function, useLists, argument);
        if (!chain.isEmpty() || !isCastOrConversion(function.at(argument).opcode))
            chain.append(argument);
    }
    if (chain.isEmpty()) {
        merged.sourcesOfOtherValues.append(edge.from);
        return true;
    }

    ValueID definition = chain.first();
    ValueLocation location = useLists.locationOf(definition);
    if (location.placement == ValuePlacement::Parameter && location.block == edge.from && location.block != entryBlock) {
        merged.isTestedAfterMerge |= hasCastWithTest(function, chain.span());
        return collectSourcesOfParameter(pass, merged, definition);
    }

    std::optional<BypassedIterator> bypassed = findBypassedIterator(pass, definition, merged.blockOfOpen);
    std::optional<unsigned> arm = bypassed ? findOrAddClassArm(pass, merged, bypassed->arm) : std::nullopt;
    if (arm)
        merged.sources.append(ArmSource { edge.from, *arm, bypassed->receiver, use });
    else
        merged.sourcesOfOtherValues.append(edge.from);
    return true;
}

static bool collectSourcesOfParameter(FunctionPassContext& pass, MergedIterators& merged, ValueID parameter)
{
    ValueLocation location = pass.analyses.useLists().locationOf(parameter);
    if (isSourceOrMergingBlock(merged, location.block))
        return false;
    merged.mergingBlocks.append(location.block);

    for (Edge edge : pass.analyses.cfg().incomingEdges(location.block)) {
        if (edge.isHandlerEdge())
            return false;
        const Arguments& arguments = targetOf(pass.function, edge).arguments;
        // What a test passes along its edge is no iterator that a call has just made.
        if (location.index >= arguments.size()) {
            if (isSourceOrMergingBlock(merged, edge.from))
                return false;
            merged.sourcesOfOtherValues.append(edge.from);
            continue;
        }
        if (!collectSourcesOfArgument(pass, merged, edge, arguments[location.index]))
            return false;
    }
    return true;
}

// A call under tests joins the call it stands in for: `m.values()` on a Map that nobody proved pristine makes its iterator in one block and calls whatever `values` holds in another.
static std::optional<IterationPlan> planIteratorsBypassedBeforeMerge(FunctionPassContext& pass, const Iteration& iteration, std::span<const ValueID> parameterAndCasts)
{
    ValueID parameter = parameterAndCasts.front();
    MergedIterators merged { .open = iteration.open, .blockOfOpen = pass.analyses.useLists().blockOf(iteration.open) };
    ValueLocation location = pass.analyses.useLists().locationOf(parameter);
    if (location.placement != ValuePlacement::Parameter || location.block != merged.blockOfOpen || location.block == entryBlock)
        return std::nullopt;
    merged.isTestedAfterMerge = hasCastWithTest(pass.function, parameterAndCasts);
    if (!collectSourcesOfParameter(pass, merged, parameter) || merged.sources.isEmpty())
        return std::nullopt;

    // The blocks in which the open is live are split and copied, and what is defined at the end of a source has to stay one definition.
    if (std::ranges::any_of(findLiveRegion(pass, iteration.open), [&](BlockID block) { return isSourceOrMergingBlock(merged, block); }))
        return std::nullopt;

    if (merged.isTestedAfterMerge) {
        for (ArmSource& source : merged.sources)
            source.passedIterator = std::nullopt;
    }

    IterationPlan plan;
    plan.source = pass.function.at(iteration.open).iterableOperand();
    plan.arms = WTF::move(merged.classArms);
    plan.armSources = WTF::move(merged.sources);
    if (!merged.sourcesOfOtherValues.isEmpty()) {
        unsigned protocolArm = plan.arms.size();
        plan.arms.append(IterationArm { });
        for (BlockID block : merged.sourcesOfOtherValues)
            plan.armSources.append(ArmSource { block, protocolArm, { }, std::nullopt });
    }
    return plan;
}

static std::optional<IterationPlan> planBypassedIterator(FunctionPassContext& pass, const Iteration& iteration)
{
    static_assert(!OperandIndex::IteratorOpen::iterable, "findSingleUseCastChain starts at operand 0.");
    const UseLists& useLists = pass.analyses.useLists();

    Vector<ValueID, 2> chain = findSingleUseCastChain(pass.function, useLists, iteration.open);
    if (chain.isEmpty())
        return std::nullopt;
    std::optional<BypassedIterator> bypassed = findBypassedIterator(pass, chain.first(), useLists.blockOf(iteration.open));
    if (!bypassed)
        return planIteratorsBypassedBeforeMerge(pass, iteration, chain.span());

    IterationPlan plan;
    plan.source = bypassed->receiver;
    plan.arms.append(bypassed->arm);
    return plan;
}

// The first search throws what the call may throw.
static bool hasNoObservableEffectBetween(FunctionPassContext& pass, ValueID call, ValueID open)
{
    const UseLists& useLists = pass.analyses.useLists();
    ValueLocation first = useLists.locationOf(call);
    ValueLocation last = useLists.locationOf(open);
    if (first.block != last.block)
        return false;

    const BlockValues& values = pass.function.at(first.block).values;
    for (uint32_t index = first.index + 1; index < last.index; ++index) {
        if (!pass.analyses.effectsOf(values[index]).onAnyEdge().isRemovableWhenUnused())
            return false;
    }
    return true;
}

// A string never changes, so its substrings are the same whenever they are cut: each step searches for the next separator, and nobody needs the array.
// A pattern without a rest element reads a prefix of the array, which the call delivers at once.
static std::optional<IterationPlan> planSplitString(FunctionPassContext& pass, const Iteration& iteration)
{
    static_assert(!OperandIndex::IteratorOpen::iterable, "findSingleUseCastChain starts at operand 0.");
    const Function& function = pass.function;

    const Value& open = function.at(iteration.open);
    if (open.isDestructuring() && !open.hasRest())
        return std::nullopt;
    Vector<ValueID, 2> chain = findSingleUseCastChain(function, pass.analyses.useLists(), iteration.open);
    if (chain.isEmpty())
        return std::nullopt;
    std::optional<SplitString> splitString = splitStringOf(pass, function.at(chain.first()));
    if (!splitString || !hasNoObservableEffectBetween(pass, chain.first(), iteration.open))
        return std::nullopt;
    if (!hasConstantIteratorMethod(pass, IterationArm { .form = IterationForm::Array, .builtinClass = BuiltinClasses::Array }))
        return std::nullopt;

    IterationPlan plan;
    plan.source = splitString->string;
    plan.arms.append(IterationArm {
        .form = IterationForm::SplitString,
        .testedType = function.at(splitString->string).type,
        .separator = splitString->separator,
        .separatorSearch = splitString->separatorSearch,
        .classIsProven = true,
        .namesAreProven = true,
    });
    plan.replacedValues = WTF::move(chain);
    return plan;
}

IterationPlan planIteration(FunctionPassContext& pass, const Iteration& iteration)
{
    bool isSentValues = std::ranges::any_of(iteration.resumes, [&](ValueID resume) {
        return !!pass.function.at(resume).sentOperand();
    });
    PlannedValue planned { pass.function.at(iteration.open).iterableOperand(), pass.analyses.useLists().blockOf(iteration.open), { PristineAspects::Names }, iteration.kind, isSentValues };
    switch (iteration.kind) {
    case IteratorKind::Sync:
        break;
    case IteratorKind::Async:
        return planForValue(pass, planned);
    }

    if (std::optional<IterationPlan> bypassed = planBypassedIterator(pass, iteration))
        return WTF::move(*bypassed);
    if (std::optional<IterationPlan> splitString = planSplitString(pass, iteration))
        return WTF::move(*splitString);
    return planForValue(pass, planned);
}

IterationPlan planSpread(FunctionPassContext& pass, ValueID appendSpread)
{
    ValueID iterable = pass.function.at(appendSpread).iterableOperand();
    BlockID block = pass.analyses.useLists().blockOf(appendSpread);
    Arms arms = findClassArms(pass, PlannedValue { iterable, block, { PristineAspects::Storage, PristineAspects::Names } });

    // An arm's class is proven relative to the tests in front of it, so the other arms go before that is decided.
    arms.removeAllMatching([](const IterationArm& arm) {
        return arm.form != IterationForm::Array;
    });
    return planWithClassArms(pass, iterable, WTF::move(arms));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
