/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTExpandIteration.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTDerivedFunctions.h"
#include "AOTEffectsCheck.h"
#include "AOTFunctionAnalyses.h"
#include "AOTResolveCallsDirectCall.h"
#include "AOTStringArena.h"
#include "AOTTypeTestSite.h"
#include <algorithm>
#include <array>
#include <functional>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct SpreadSite {
    ProgramPoint point;
    ValueID value;
    IterationPlan plan;
};

struct SpreadExpansion {
    FunctionPassContext& pass;
    IRBuilder& builder;
    IRBuilder::Expansion cut;
    ValueID array;
    BlockID loopEntry;
};

struct SpreadLoop {
    ValueID open;
    ValueID array;
    BlockID entry;
    BlockID continuation;
    std::optional<Target> handler;
    std::optional<DerivedFunctionKey> outlinedLoop;
};

} // anonymous namespace

static Vector<SpreadSite> collectSpreads(FunctionPassContext& pass)
{
    Vector<SpreadSite> sites;
    for (BlockID block : pass.analyses.cfg().reversePostOrder()) {
        const BlockValues& values = pass.function.at(block).values;
        for (uint32_t index = 0; index < values.size(); ++index) {
            if (pass.function.at(values[index]).opcode == Opcode::AppendSpread)
                sites.append(SpreadSite { ProgramPoint { block, index }, values[index], planSpread(pass, values[index]) });
        }
    }
    return sites;
}

static ReplacedEffects replacedEffectsOf(FunctionPassContext& pass, const Value& appendSpread)
{
    ReplacedEffects replaced;
    replaced.add(pass.analyses.effectContext(), pass.function, appendSpread);
    return replaced;
}

static void checkAppendElements(FunctionPassContext& pass, BlockID block, ValueID appendElements, const ReplacedEffects& replaced)
{
    checkReplacement(pass.analyses.effectContext(), pass.function, { pass.id, block, appendElements }, pass.function.at(appendElements), replaced, pass.build.diagnostics());
}

static ValueID appendResultAt(IRBuilder& builder, ValueID step, IteratorResumeResult result)
{
    Value read = IRBuilder::make(Opcode::ResultAt, { step });
    read.setIndex(static_cast<uint32_t>(result));
    return builder.append(WTF::move(read));
}

static void replaceWithAppendElements(FunctionPassContext& pass, const SpreadSite& site)
{
    Value& value = pass.function.edit(site.value);
    ReplacedEffects replaced = replacedEffectsOf(pass, value);
    ValueID array = value.arrayOperand();
    ValueID source = value.iterableOperand();
    value.become(Opcode::AppendElements);
    value.setArrayOperand(array);
    value.setSourceOperand(source);
    pass.analyses.didChangeValues();
    checkAppendElements(pass, site.point.block, site.value, replaced);
}

static ValueID addLoopEntry(SpreadExpansion& expansion, BlockFrequency frequency, ValueID iterable)
{
    Function& function = expansion.pass.function;
    TypeID expected = function.at(iterable).expected();
    expansion.loopEntry = expansion.builder.addBlock(frequency);
    ValueID source = expansion.builder.addParameter(expansion.loopEntry, function.at(iterable).type);
    if (expected) {
        function.edit(source, ValueEdit::Type).setExpected(expected);
        expansion.pass.analyses.didChangeValues();
    }
    return source;
}

static void emitFailedClassTest(SpreadExpansion& expansion, ValueID rest)
{
    IRBuilder& builder = expansion.builder;
    if (expansion.loopEntry) {
        builder.emitJump(expansion.loopEntry, { rest });
        return;
    }

    Value check = IRBuilder::make(Opcode::RequireObjectCoercible, { rest });
    check.setNullishAccess(NullishAccess::Spread);
    builder.append(WTF::move(check));
    builder.emitUnreachable();
}

static void emitArrayArm(SpreadExpansion& expansion, const IterationArm& arm, ValueID iterable, const ReplacedEffects& replaced)
{
    RELEASE_ASSERT(arm.form == IterationForm::Array);
    IRBuilder& builder = expansion.builder;
    ValueID subject = iterable;
    ValueID guard;

    if (!arm.classIsProven) {
        ValueID test = builder.appendTypeTest(iterable, arm.testedType);
        IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(test, OperandIndex::TypeTest::value, BlockFrequency::Normal, BlockFrequency::Unlikely);
        subject = branch.provenOnTaken;
        builder.moveToEndOf(branch.notTaken);
        emitFailedClassTest(expansion, branch.provenOnNotTaken);
        builder.moveToEndOf(branch.taken);
    }

    if (!arm.namesAreProven) {
        RELEASE_ASSERT(expansion.loopEntry);
        ValueID test = builder.append(makePristineTest(subject, BuiltinClasses::Array, { PristineAspects::Storage, PristineAspects::Names }));
        IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(test, 0, BlockFrequency::Normal, BlockFrequency::Unlikely);
        guard = branch.provenOnTaken;
        builder.moveToEndOf(branch.notTaken);
        builder.emitJump(expansion.loopEntry, { chooseArgumentOfProtocolEntry(expansion.pass, expansion.loopEntry, subject, iterable) });
        builder.moveToEndOf(branch.taken);
    }

    Value appendElements = IRBuilder::make(Opcode::AppendElements, { expansion.array, subject });
    if (guard)
        appendElements.setGuardOperand(guard);
    BlockID block = builder.currentBlock();
    checkAppendElements(expansion.pass, block, builder.append(WTF::move(appendElements)), replaced);
    builder.endExpansion(expansion.cut, std::span<const ValueID> { });
}

static SpreadLoop emitOpen(SpreadExpansion& expansion, ValueID source)
{
    IRBuilder& builder = expansion.builder;
    builder.moveToEndOf(expansion.loopEntry);
    Value iteratorOpen = IRBuilder::make(Opcode::IteratorOpen, { source });
    iteratorOpen.setIteratorKind(IteratorKind::Sync);
    ValueID open = builder.append(WTF::move(iteratorOpen));
    builder.endExpansion(expansion.cut, std::span<const ValueID> { });
    return { open, expansion.array, expansion.loopEntry, expansion.cut.continuation, expansion.cut.handler, std::nullopt };
}

static void emitLoop(FunctionPassContext& pass, IRBuilder& builder, const SpreadLoop& loop)
{
    BlockFrequency frequency = pass.function.at(loop.entry).frequency;
    builder.setOrigin(pass.function.at(loop.open).origin());
    builder.setHandler(loop.handler);
    BlockID header = builder.addBlock(frequency);
    builder.redirectEdge(Edge { loop.entry, 0 }, header, { });

    builder.moveToEndOf(header);
    builder.append(Opcode::LoopHint, { });
    Value iteratorResume = IRBuilder::make(Opcode::IteratorResume, { loop.open });
    iteratorResume.setIteratorKind(IteratorKind::Sync);
    iteratorResume.setResumeMode(ResumeMode::Next);
    ValueID step = builder.append(WTF::move(iteratorResume));
    ValueID done = appendResultAt(builder, step, IteratorResumeResult::Done);
    ValueID value = appendResultAt(builder, step, IteratorResumeResult::Value);
    IRBuilder::BranchBlocks blocks = builder.emitBranch(done, frequency, frequency);
    builder.emitJump(loop.continuation);

    builder.moveToEndOf(blocks.notTaken);
    builder.append(Opcode::AppendElement, { loop.array, value });
    builder.emitJump(header);
}

static constexpr unsigned maxExpectedClassesInKey = 3;
static constexpr unsigned bitsOfClassInKey = 10;

static DerivedFunctionKey keyOfAppendIterable(std::span<const BuiltinClassID> sortedExpectedClasses)
{
    RELEASE_ASSERT(sortedExpectedClasses.size() <= maxExpectedClassesInKey);
    uint32_t packedClasses = 0;
    for (BuiltinClassID expectedClass : sortedExpectedClasses) {
        RELEASE_ASSERT(expectedClass.index() + 1 < 1u << bitsOfClassInKey);
        packedClasses = packedClasses << bitsOfClassInKey | (expectedClass.index() + 1);
    }
    return { .role = FunctionRole::OutlinedBody, .target = FunctionID(), .parameter = packedClasses, .node = static_cast<uint32_t>(OutlinedIteration::AppendIterable) };
}

bool isOutlinedIteration(const Function& function)
{
    return function.role == FunctionRole::OutlinedBody && function.derivation.node;
}

Function createOutlinedIteration(DerivedFunctionInputs& inputs, ASCIILiteral name, std::span<const TypeID> parameters, std::span<const TypeID> results)
{
    Function function;
    function.name = inputs.strings.intern(StringView(name));
    function.kind = FunctionKind::Plain;
    function.role = FunctionRole::OutlinedBody;
    function.length = static_cast<uint32_t>(parameters.size());
    function.thisType = DeclaredType { .type = TypeTable::anything };
    for (TypeID type : parameters) {
        function.parameters.append(DeclaredType { .type = type });
        function.derivation.parameterMap.append(ParameterOrigin { });
    }
    for (TypeID type : results)
        function.results.append(DeclaredType { .type = type });
    function.capabilities = initialCapabilitiesOf(function);
    return function;
}

static Type iterablesExpectedBy(const DerivedFunctionKey& key, TypeArena& types)
{
    Type iterables = Type::builtinClass(BuiltinClasses::Array);
    if (!key.parameter) {
        for (BuiltinClassID builtinClass : { BuiltinClasses::Set, BuiltinClasses::Map, BuiltinClasses::Generator })
            iterables = iterables.unionWith(Type::builtinClass(builtinClass), types);
        return iterables;
    }
    for (uint32_t packedClasses = key.parameter; packedClasses; packedClasses >>= bitsOfClassInKey)
        iterables = iterables.unionWith(Type::builtinClass(BuiltinClassID((packedClasses & ((1u << bitsOfClassInKey) - 1)) - 1)), types);
    return iterables;
}

Function buildOutlinedIteration(const DerivedFunctionKey& key, DerivedFunctionInputs& inputs)
{
    if (key.node != static_cast<uint32_t>(OutlinedIteration::AppendIterable))
        return buildSharedProtocolFunction(static_cast<OutlinedIteration>(key.node), inputs);

    TypeID arrayType = inputs.types.intern(Type::builtinClass(BuiltinClasses::Array));
    Function function = createOutlinedIteration(inputs, "appendIterable"_s, std::array { arrayType, TypeTable::anything }, std::array { TypeTable::undefined });

    IRBuilder builder({ inputs.build, inputs.program, &inputs.flow, nullptr }, function, inputs.types);
    BlockID entry = builder.addBlock();
    ValueID array = builder.addParameter(entry, arrayType);
    ValueID iterable = builder.addParameter(entry, TypeTable::anything);
    function.edit(iterable, ValueEdit::Type).setExpected(inputs.types.intern(iterablesExpectedBy(key, inputs.types)));
    builder.moveToEndOf(entry);
    builder.append(Opcode::AppendSpread, { array, iterable });
    builder.emitReturn(builder.appendUndefined());
    return function;
}

static void replaceWithCallOfOutlinedLoop(FunctionPassContext& pass, IRBuilder& builder, const SpreadLoop& loop)
{
    Function& function = pass.function;
    RELEASE_ASSERT(function.at(loop.entry).values.size() == 1 && function.at(loop.entry).values[0] == loop.open);
    ValueID source = function.at(loop.open).iterableOperand();
    BlockFrequency frequency = function.at(loop.entry).frequency;
    builder.beginExpansion(ProgramPoint::atHeadOf(loop.entry));

    if (pass.program.typeOf(function, source).mayBeUndefinedOrNull()) {
        ValueID test = builder.appendTypeTest(source, TypeTable::nullish);
        IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(test, OperandIndex::TypeTest::value, BlockFrequency::Rare, frequency);
        Value check = IRBuilder::make(Opcode::RequireObjectCoercible, { branch.provenOnTaken });
        check.setNullishAccess(NullishAccess::Spread);
        builder.append(WTF::move(check));
        builder.emitUnreachable();
        builder.moveToEndOf(branch.notTaken);
        source = branch.provenOnNotTaken;
    }

    std::array<ValueID, 2> arguments { loop.array, source };
    appendDirectCall(builder, { .callee = pass.derivedFunction(*loop.outlinedLoop), .arguments = arguments, .isNeverInlined = true });
    builder.emitJump(loop.continuation);
}

static std::optional<DerivedFunctionKey> findOutlinedLoop(FunctionPassContext& pass, const SpreadLoop& loop)
{
    if (isOutlinedIteration(pass.function))
        return std::nullopt;
    if (pass.program.typeOf(pass.function, pass.function.at(loop.open).iterableOperand()).layouts)
        return std::nullopt;
    IterationPlan plan = planIteration(pass, Iteration { .open = loop.open, .origin = IterationOrigin::Spread });
    if (!plan.hasProtocolArm() || !plan.armSources.isEmpty())
        return std::nullopt;

    Vector<BuiltinClassID, maxExpectedClassesInKey> expectedClasses;
    for (const IterationArm& arm : plan.arms) {
        if (arm.builtinClass && arm.builtinClass != BuiltinClasses::Array && expectedClasses.size() < maxExpectedClassesInKey)
            expectedClasses.append(arm.builtinClass);
    }
    std::ranges::sort(expectedClasses, { }, &BuiltinClassID::index);
    return keyOfAppendIterable(expectedClasses.span());
}

static void finishLoops(FunctionPassContext& pass, IRBuilder& builder, Vector<SpreadLoop>& loops, Vector<ValueID>& opensOfSpreads)
{
    for (SpreadLoop& loop : loops)
        loop.outlinedLoop = findOutlinedLoop(pass, loop);
    for (const SpreadLoop& loop : loops) {
        if (loop.outlinedLoop) {
            replaceWithCallOfOutlinedLoop(pass, builder, loop);
            continue;
        }
        emitLoop(pass, builder, loop);
        opensOfSpreads.append(loop.open);
    }
    builder.emptyUnreachableBlocks();
}

static void expandSpread(FunctionPassContext& pass, IRBuilder& builder, const SpreadSite& site, Vector<SpreadLoop>& loops)
{
    const IterationPlan& plan = site.plan;
    RELEASE_ASSERT(plan.hasClassArm() || plan.hasProtocolArm());
    if (plan.hasClassArm() && plan.arms.first().needsNoTest()) {
        replaceWithAppendElements(pass, site);
        return;
    }

    Value original = pass.function.at(site.value);
    ValueID iterable = original.iterableOperand();
    ReplacedEffects replaced = replacedEffectsOf(pass, original);
    BlockFrequency loopFrequency = rarerOf(plan.hasClassArm() ? BlockFrequency::Unlikely : BlockFrequency::Normal, pass.function.at(site.point.block).frequency);

    builder.setOrigin(original.origin());
    SpreadExpansion expansion { pass, builder, builder.beginExpansion(site.point), original.arrayOperand(), { } };
    ValueID source;
    if (plan.hasProtocolArm())
        source = addLoopEntry(expansion, loopFrequency, iterable);

    if (plan.hasClassArm())
        emitArrayArm(expansion, plan.arms.first(), iterable, replaced);
    else
        builder.emitJump(expansion.loopEntry, { iterable });

    if (expansion.loopEntry)
        loops.append(emitOpen(expansion, source));
}

PassOutcome expandSpreads(FunctionPassContext& pass, IRBuilder& builder, Vector<ValueID>& opensOfSpreads)
{
    Vector<SpreadSite> sites = collectSpreads(pass);
    if (sites.isEmpty())
        return PassOutcome::Unchanged;

    std::ranges::sort(sites, std::ranges::greater { }, [](const SpreadSite& site) {
        return std::pair { site.point.block, site.point.index };
    });
    Vector<SpreadLoop> loops;
    for (const SpreadSite& site : sites)
        expandSpread(pass, builder, site, loops);
    finishLoops(pass, builder, loops, opensOfSpreads);
    builder.setHandler(std::nullopt);
    return PassOutcome::Changed;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
