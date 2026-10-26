/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTProgramPasses.h"

#if ENABLE(AOT)

#include "AOTBlockFrequencies.h"
#include "AOTBuildContext.h"
#include "AOTBuiltinCallPlan.h"
#include "AOTBuiltinCalls.h"
#include "AOTBuiltinMembers.h"
#include "AOTBuiltinPristine.h"
#include "AOTBuiltinTable.h"
#include "AOTCodeSizeEstimate.h"
#include "AOTCoroutines.h"
#include "AOTDerivedFunctions.h"
#include "AOTEffectsCheck.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsOfAccesses.h"
#include "AOTExpandOwnKeysInternal.h"
#include "AOTFlowApply.h"
#include "AOTFunctionAnalyses.h"
#include "AOTFuseGeneratorLoops.h"
#include "AOTIRBuilder.h"
#include "AOTPass.h"
#include "AOTPropertyAccess.h"
#include "AOTPropertyAccessReceivers.h"
#include "AOTRegionCloner.h"
#include "AOTResolveCallsDirectCall.h"
#include "AOTResultRulesInternal.h"
#include "AOTRuleSupport.h"
#include "AOTSSARepair.h"
#include "AOTSort.h"
#include "AOTSplitStringWalk.h"
#include "AOTSplitUnions.h"
#include "AOTStringArena.h"
#include "AOTTypeTestSite.h"
#include "AOTValueIdentity.h"
#include "IterationKind.h"
#include "JSArrayIterator.h"
#include "TypedArrayType.h"
#include <algorithm>
#include <array>
#include <functional>
#include <memory>
#include <ranges>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(std::ranges::all_of(allMembersOf<Opcode>, [](Opcode opcode) {
    return removedByOf(opcode) != PassID::ExpandIteration || expandedOpcodes.contains(opcode);
}));
static_assert(!OperandIndex::IteratorOpen::iterable, "findSingleUseCastChain starts at operand 0.");
static_assert(static_cast<uint32_t>(StepArrayIteratorResult::Done) == static_cast<uint32_t>(IteratorResumeResult::Done));
static_assert(static_cast<uint32_t>(StepArrayIteratorResult::Value) == static_cast<uint32_t>(IteratorResumeResult::Value));

namespace {

enum class IterationForm : uint8_t { Protocol, Array, TypedArray, String, SplitString, OwnEntries, Map, Set, Generator, AsyncGenerator };
enum class IterationOrigin : uint8_t { Source, Spread, Delegation };
enum class PatternRead : uint8_t { BySteps, ByIndex, InOneRead };
// Proven: the plan has one arm, the arm has no test, and closing does nothing.
enum class ExpandedIterations : uint8_t { All, OnlyProven };

constexpr uint32_t doneResult = static_cast<uint32_t>(IteratorResumeResult::Done);
constexpr uint32_t valueResult = static_cast<uint32_t>(IteratorResumeResult::Value);

struct IterationArm {
    IterationForm form { IterationForm::Protocol };
    JSC::IterationKind kind { JSC::IterationKind::Values };
    BuiltinClassID builtinClass;
    JSC::TypedArrayType typedArrayType { JSC::NotTypedArray };
    TypeID testedType;
    ValueID separator;
    BuiltinOverloadID separatorSearch;
    bool classIsProven { false };
    bool namesAreProven { false };
    bool bypassesIterator { false };
    // The protocol arm of a value that may be a string nobody expected.
    bool alsoServesStrings { false };
    // A class arm of an async iteration: it stands for the wrapper around the synchronous iterator.
    bool awaitsEachValue { false };

    bool needsNoTest() const { return classIsProven && namesAreProven; }
    bool hasMethodsToCall() const { return form == IterationForm::Protocol || form == IterationForm::Generator || form == IterationForm::AsyncGenerator; }
    // An arm that awaits each value stands for the wrapper around a synchronous iterator, and closing the wrapper takes a turn of the job queue.
    bool closingHasNoEffect() const { return !hasMethodsToCall() && !awaitsEachValue; }

    friend bool operator==(const IterationArm&, const IterationArm&) = default;
};

// Control that leaves this block for the open takes this arm. A class arm walks the subject, and nobody needs the iterator that was passed on.
struct ArmSource {
    BlockID block;
    unsigned arm { 0 };
    ValueID subject;
    std::optional<Use> passedIterator;
};

using Arms = Vector<IterationArm, 2>;

struct IterationPlan {
    ValueID source;
    Arms arms;
    Vector<ValueID, 2> replacedValues;
    // Where the iterable merges what several blocks made: control comes through one of them last, so no arm needs a test.
    Vector<ArmSource, 2> armSources;

    bool hasProtocolArm() const { return !arms.isEmpty() && arms.last().form == IterationForm::Protocol; }
    bool hasClassArm() const { return !arms.isEmpty() && arms.first().form != IterationForm::Protocol; }
    std::span<const IterationArm> classArms() const LIFETIME_BOUND { return arms.span().first(arms.size() - hasProtocolArm()); }
    bool isExpandedWithoutTest() const { return arms.size() == 1 && armSources.isEmpty() && arms.first().needsNoTest() && arms.first().closingHasNoEffect(); }
};

struct Iteration {
    ValueID open;
    IteratorKind kind { IteratorKind::Sync };
    IterationOrigin origin { IterationOrigin::Source };
    Vector<ValueID, 2> resumes;
    Vector<ValueID, 2> closes;
    Vector<BlockID, 1> cleanupBlocks;
};

struct PlannedValue {
    ValueID value;
    BlockID block;
    OptionSet<PristineAspects> aspects;
    IteratorKind kind { IteratorKind::Sync };
    bool isSentValues { false };
    bool isOnlyAskedForNextValue { true };
};

struct BypassedIterator {
    ValueID receiver;
    IterationArm arm;
};

struct PatternInOneRead {
    Iteration operations;
    ValueID iterable;
};

struct SpreadLoop {
    ValueID open;
    ValueID array;
    BlockID entry;
    BlockID continuation;
    std::optional<Target> handler;
    std::optional<DerivedFunctionKey> outlinedLoop;
};

struct KeyedOperation {
    Opcode opcodeWhenCurrent;
    ValueID object;
    ValueID key;
};

struct OpenedIterator {
    ValueID iterator;
    ValueID next;
    ValueID isSteppedWithoutCall;
    bool isOpenedBySharedFunction { false };
};

struct StateVariable {
    SSARepair::Variable variable { 0 };
    ValueID anyDefinition;
};

struct ArmState {
    IterationArm arm;
    PatternRead patternRead { PatternRead::BySteps };
    ValueID subject;
    ValueID nextMethod;
    ValueID isSteppedWithoutCall;
    bool callsSharedFunctions { false };
    std::optional<StateVariable> position;
    std::optional<StateVariable> table;
};

struct StepRequest {
    IteratorKind kind { IteratorKind::Sync };
    ResumeMode mode { ResumeMode::Next };
    ValueID sent;
    bool readsValueWhenDone { false };
};

template<typename Attribute>
Value makeWith(Opcode opcode, std::initializer_list<ValueID> operands, void (Value::*set)(Attribute), std::type_identity_t<Attribute> attribute)
{
    Value value = IRBuilder::make(opcode, operands);
    (value.*set)(attribute);
    return value;
}

// What emits the operations of an iteration: into the function that iterates, or, without a pass, into a function that many iterations share, where an operation ends by returning its results.
struct IterationExpansion {
    FunctionPassContext* pass;
    const Program& program;
    Function& function;
    TypeArena& types;
    IRBuilder& builder;
    SSARepair* repair;
    IterationOrigin origin { IterationOrigin::Source };
    const IRBuilder::Expansion* operation { nullptr };
    ValueID positionInFrontOfStep { };
    ValueID positionBehindStep { };

    bool isMakingSharedFunction() const { return !pass; }
    bool isExpectedToRun() const { return isMakingSharedFunction() || function.at(builder.currentBlock()).frequency == BlockFrequency::Normal; }
    StateVariable addVariable(ValueID initialDefinition) { return { repair->addVariable(initialDefinition), initialDefinition }; }

    void define(const StateVariable& variable, ValueID definition)
    {
        if (isMakingSharedFunction()) {
            RELEASE_ASSERT(positionInFrontOfStep);
            positionBehindStep = definition;
        } else
            repair->addDefinition(variable.variable, definition);
    }

    void finishStep(ValueID done, ValueID value)
    {
        static_assert(!doneResult && valueResult == 1);
        if (isMakingSharedFunction()) {
            RELEASE_ASSERT(positionInFrontOfStep);
            builder.emitReturn(builder.append(Opcode::Results, { done, value, std::exchange(positionBehindStep, positionInFrontOfStep) }));
            return;
        }
        RELEASE_ASSERT(operation && operation->results.size() == 2);
        builder.endExpansion(*operation, std::array { done, value });
    }

    void finishStepAsDone()
    {
        ValueID done = builder.appendBoolean(true);
        finishStep(done, builder.appendUndefined());
    }

    void finishStepAsDoneIn(BlockID block)
    {
        builder.moveToEndOf(block);
        finishStepAsDone();
    }

    void finishClose()
    {
        if (isMakingSharedFunction()) {
            builder.emitReturn(builder.appendUndefined());
            return;
        }
        RELEASE_ASSERT(operation);
        builder.endExpansion(*operation, std::span<const ValueID> { });
    }

    BlockID addSiblingBlock() { return builder.addBlock(function.at(builder.currentBlock()).frequency); }
    PropertyKeyID keyOf(WellKnownSymbol symbol) { return types.wellKnownSymbolKey(symbol); }
    PropertyKeyID keyOf(BuiltinName name) { return program.keyOf(name); }

    ValueID appendResultAt(ValueID producer, auto index) { return builder.append(makeResultAt(producer, static_cast<uint32_t>(index))); }
    ValueID appendIncrement(ValueID integer, int64_t amount) { return builder.append(Opcode::Add, { integer, builder.appendInteger(amount) }); }
    ValueID appendIsEqualToInteger(ValueID value, auto constant) { return builder.append(Opcode::StrictEq, { value, builder.appendInteger(static_cast<int64_t>(constant)) }); }
    ValueID appendBuiltin(BuiltinID id) { return builder.append(makeWith(Opcode::Builtin, { }, &Value::setBuiltin, id)); }
    ValueID appendPropGet(ValueID object, auto name) { return builder.append(makeWith(Opcode::PropGet, { object }, &Value::setKey, keyOf(name))); }
    ValueID appendLength(ValueID object) { return builder.append(Opcode::ToLength, { appendPropGet(object, BuiltinNames::length) }); }
    ValueID appendElement(ValueID object, ValueID index) { return builder.append(makeWith(Opcode::ElemGet, { object, index }, &Value::setKeyIsIntegral, true)); }
    ValueID appendRequireObject(ValueID value, RuntimeErrorKind kind) { return builder.append(makeWith(Opcode::RequireObject, { value }, &Value::setRuntimeError, kind)); }
    ValueID appendUnspecifiedLike(ValueID model) { return builder.append(makeWith(Opcode::Unspecified, { }, &Value::setValueType, function.at(model).type)); }
    ValueID appendAwaitIfAsync(IteratorKind kind, ValueID value) { return kind == IteratorKind::Async ? appendAwait(builder, value) : value; }
    ValueID appendIteratorMethodCall(ValueID method, ValueID iterable, RuntimeErrorKind errorKind = RuntimeErrorKind::IteratorIsNotAnObject) { return appendRequireObject(appendCall(method, iterable, { }), errorKind); }
    IRBuilder::ProvenBranch branchOnInRange(const ArmState& state, ValueID length) { return builder.emitBranchWithProof(builder.append(Opcode::Less, { state.position->anyDefinition, length })); }
    IRBuilder::BranchBlocks branchOnNotStepped(ValueID status) { return builder.emitBranch(appendIsEqualToInteger(status, GeneratorStepStatus::NotStepped), BlockFrequency::Rare, BlockFrequency::Normal); }
    void emitThrow(RuntimeErrorKind kind) { builder.emitThrow(builder.append(makeWith(Opcode::NewRuntimeError, { }, &Value::setRuntimeError, kind))); }
    void emitThrowOfMissingThrowMethod() { emitThrow(RuntimeErrorKind::IteratorWithoutThrowMethod); }

    ValueID appendCall(ValueID callee, ValueID thisValue, std::span<const ValueID> arguments)
    {
        Value call(Opcode::Call);
        call.setCalleeOperand(callee);
        call.setThisValueOperand(thisValue);
        call.setArgumentsOperands(arguments);
        return builder.append(WTF::move(call));
    }

    ValueID appendPair(ValueID first, ValueID second)
    {
        Value pair(Opcode::NewArray);
        pair.setContents(TypeTable::anything);
        pair.setElementsOperands(std::array { first, second });
        return builder.append(WTF::move(pair));
    }

    ValueID appendValueOfKind(JSC::IterationKind kind, ValueID key, ValueID value)
    {
        if (kind == JSC::IterationKind::Entries)
            return appendPair(key, value);
        return kind == JSC::IterationKind::Keys ? key : value;
    }

    IRBuilder::ProvenBranch branchOnType(ValueID value, TypeID type, BlockFrequency taken = BlockFrequency::Normal, BlockFrequency notTaken = BlockFrequency::Normal)
    {
        return builder.emitBranchWithProof(builder.appendTypeTest(value, type), 0, taken, notTaken);
    }
};

} // anonymous namespace

static Value makeRequireObjectCoercible(ValueID value, IterationOrigin origin)
{
    return makeWith(Opcode::RequireObjectCoercible, { value }, &Value::setNullishAccess, origin == IterationOrigin::Spread ? NullishAccess::Spread : NullishAccess::Iterate);
}

static Value makeNamesTest(const IterationArm& arm, ValueID subject, OptionSet<PristineAspects> aspects)
{
    if (arm.form == IterationForm::Generator)
        return IRBuilder::make(Opcode::IsUnmodifiedGenerator, { subject });
    if (arm.form == IterationForm::AsyncGenerator)
        return IRBuilder::make(Opcode::IsUnmodifiedAsyncGenerator, { subject });
    return makePristineTest(subject, arm.builtinClass, aspects);
}

static std::pair<BlockID, uint32_t> blockAndIndexOf(ProgramPoint point)
{
    return { point.block, point.index };
}

static BitVector blockSetOf(const Function& function, std::span<const BlockID> blocks)
{
    BitVector set(function.blocks.size());
    for (BlockID block : blocks)
        set.quickSet(block.index());
    return set;
}

static ValueID addParameterLike(FunctionPassContext& pass, IRBuilder& builder, BlockID block, ValueID model)
{
    TypeID expected = pass.function.at(model).expected();
    ValueID parameter = builder.addParameter(block, pass.function.at(model).type);
    if (expected) {
        pass.function.edit(parameter, ValueEdit::Type).setExpected(expected);
        pass.analyses.didChangeValues();
    }
    return parameter;
}

// What a passed test leaves of a type may lie outside it: beside a class that may be related, the part of the tested class loses what it proves.
static ValueID chooseArgumentOfProtocolEntry(FunctionPassContext& pass, BlockID protocolEntry, ValueID narrowed, ValueID tested)
{
    TypingContext typing { pass.program, pass.function, pass.types };
    return typing.typeOf(narrowed).isSubsetOf(typing.typeOf(pass.function.at(protocolEntry).parameters[0]), pass.program) ? narrowed : tested;
}

bool isOutlinedIteration(const Function& function)
{
    return function.role == FunctionRole::OutlinedBody && function.derivation.node;
}

// An iterator is the base of no interior pointer and no test narrows it, so it is live from its open to its uses.
static Vector<BlockID> findLiveRegion(FunctionPassContext& pass, ValueID open)
{
    const CFG& cfg = pass.analyses.cfg();
    const UseLists& useLists = pass.analyses.useLists();
    BlockID blockOfOpen = useLists.blockOf(open);

    Vector<BlockID> region;
    BitVector isLiveAtHead(pass.function.blocks.size());
    Vector<BlockID, 16> worklist;
    for (const Use& use : useLists.usesOf(open))
        worklist.append(use.block);
    while (!worklist.isEmpty()) {
        BlockID block = worklist.takeLast();
        if (block == blockOfOpen || !cfg.isReachable(block) || isLiveAtHead.quickSet(block.index()))
            continue;
        region.append(block);
        worklist.append(cfg.predecessors(block));
    }
    AOT::sort(region, { }, [&](BlockID block) { return cfg.reversePostOrderIndex(block); });
    return region;
}

// The caller asks until nothing is left, so its last call comes right after the function has changed. That call answers without computing any analysis.
static Vector<Iteration> collectIterations(FunctionPassContext& pass)
{
    const Function& function = pass.function;
    if (std::ranges::none_of(function.blocks, [&](const Block& block) { return block.values.containsIf([&](ValueID id) { return function.at(id).opcode == Opcode::IteratorOpen; }); }))
        return { };

    Vector<Iteration> iterations;
    forEachValueSite(pass.analyses, function, { Opcode::IteratorOpen }, [&](const ValueSite& open) {
        Iteration iteration { .open = open.value, .kind = function.at(open.value).iteratorKind() };
        if (function.at(open.value).isDelegating())
            iteration.origin = IterationOrigin::Delegation;
        for (const Use& use : pass.analyses.useLists().usesOf(open.value)) {
            if (use.kind == Use::Kind::BlockSubject) {
                RELEASE_ASSERT(function.at(use.block).role == BlockRole::IterationCleanup);
                iteration.cleanupBlocks.append(use.block);
                continue;
            }
            RELEASE_ASSERT(use.kind == Use::Kind::Operand);
            if (function.at(use.user).opcode == Opcode::IteratorResume) {
                RELEASE_ASSERT(use.operandIndex == OperandIndex::IteratorResume::iterator);
                iteration.resumes.append(use.user);
            } else {
                RELEASE_ASSERT(function.at(use.user).opcode == Opcode::IteratorClose && use.operandIndex == OperandIndex::IteratorClose::iterator);
                iteration.closes.append(use.user);
            }
        }
        iterations.append(WTF::move(iteration));
    });
    return iterations;
}

// Which arms serve an iteration.

// An async iteration over an object without @@asyncIterator wraps the iterator that @@iterator makes.
static std::optional<IterationForm> formOfClass(const BuiltinTable& builtins, BuiltinClassID id, IteratorKind kind)
{
    if (kind == IteratorKind::Sync && id == BuiltinClasses::Generator)
        return IterationForm::Generator;
    if (kind == IteratorKind::Async && id == BuiltinClasses::AsyncGenerator)
        return IterationForm::AsyncGenerator;
    if (id == BuiltinClasses::Array)
        return IterationForm::Array;
    if (id == BuiltinClasses::Map)
        return IterationForm::Map;
    if (id == BuiltinClasses::Set)
        return IterationForm::Set;
    if (isTypedView(builtins.at(id).typedArrayType))
        return IterationForm::TypedArray;
    return std::nullopt;
}

// Flow proves an instance pristine from what is done to the instance. What its prototypes hold under @@iterator has a proof of its own.
static bool hasConstantIteratorMethod(FunctionPassContext& pass, const IterationArm& arm)
{
    const Program& program = pass.program;
    auto findMethod = [&](WellKnownSymbol symbol) {
        return findMemberOfInstances({ program, pass.inputs.flow }, arm.form == IterationForm::String ? BuiltinClasses::String : arm.builtinClass, program.keyOf(program.builtins().nameOf(symbol)));
    };
    if (arm.form == IterationForm::AsyncGenerator)
        return findMethod(WellKnownSymbol::AsyncIterator).isConstant();
    if (arm.awaitsEachValue) {
        BuiltinMemberAnswer asyncMethod = findMethod(WellKnownSymbol::AsyncIterator);
        if (asyncMethod.outcome != BuiltinMemberLookup::Outcome::Absent || asyncMethod.proof != LocationProof::Constant)
            return false;
    }
    return findMethod(WellKnownSymbol::Iterator).isConstant();
}

static Arms collectCandidates(FunctionPassContext& pass, const PlannedValue& planned)
{
    const BuiltinTable& builtins = pass.program.builtins();
    Arms candidates;
    auto add = [&](IterationArm&& arm) {
        if (!candidates.containsIf([&](const IterationArm& other) { return other.form == arm.form && other.builtinClass == arm.builtinClass; }))
            candidates.append(WTF::move(arm));
    };
    auto addClass = [&](const ClassPart& part) {
        std::optional<IterationForm> form = formOfClass(builtins, part.builtinClass, planned.kind);
        if (!form)
            return;
        add(IterationArm {
            .form = *form,
            .kind = form == IterationForm::Map ? JSC::IterationKind::Entries : JSC::IterationKind::Values,
            .builtinClass = part.builtinClass,
            .typedArrayType = builtins.at(part.builtinClass).typedArrayType,
            .testedType = pass.types.intern(Type::builtinClass(part.builtinClass, part.contents.span())),
            .awaitsEachValue = planned.kind == IteratorKind::Async && form != IterationForm::AsyncGenerator,
        });
    };
    auto addClassWithoutContents = [&](BuiltinClassID builtinClass) {
        ClassPart part;
        part.builtinClass = builtinClass;
        addClass(part);
    };

    const Type& expected = pass.program.expectedOrTypeOf(pass.function, planned.value);
    for (const Type* type : { &expected, &pass.program.typeOf(pass.function, planned.value) }) {
        for (const ClassPart& part : type->classes)
            addClass(part);
        if (planned.kind == IteratorKind::Sync && type->primitives & Primitive::AnyString && !type->withPrimitives(Primitive::UndefinedOrNull).isAnything())
            add(IterationArm { .form = IterationForm::String, .testedType = TypeTable::string });
    }
    if (!expected.anyObject)
        return candidates;

    // What an iterable that nobody knows anything about is tested for. Of the classes with an arm only a generator does anything with what it is sent.
    if (planned.kind == IteratorKind::Async)
        addClassWithoutContents(BuiltinClasses::AsyncGenerator);
    else {
        if (planned.isSentValues)
            addClassWithoutContents(BuiltinClasses::Generator);
        addClassWithoutContents(BuiltinClasses::Array);
    }
    return candidates;
}

static bool canServe(FunctionPassContext& pass, const PlannedValue& planned, const IterationArm& arm)
{
    const Program& program = pass.program;
    auto hasFailedEarlier = [&](const Value& test) { return pass.analyses.findEarlierTest(test, planned.block) == EarlierTest::Failed; };
    if (!program.typeOf(pass.function, planned.value).overlaps(pass.types.at(arm.testedType), program) || !hasConstantIteratorMethod(pass, arm))
        return false;
    if (!arm.namesAreProven) {
        RELEASE_ASSERT(arm.builtinClass);
        if (arm.form != IterationForm::Generator && arm.form != IterationForm::AsyncGenerator && pristineTestOf(program.builtins(), arm.builtinClass) != PristineTest::MayPass)
            return false;
    }
    if (hasFailedEarlier(makeTypeTest(planned.value, arm.testedType)))
        return false;
    return arm.namesAreProven || !hasFailedEarlier(makeNamesTest(arm, planned.value, planned.aspects));
}

static Arms findClassArms(FunctionPassContext& pass, const PlannedValue& planned)
{
    Arms arms;
    for (IterationArm& arm : collectCandidates(pass, planned)) {
        arm.namesAreProven = arm.form == IterationForm::String
            || pristineAspectsProvenBy(pass.program.typeOf(pass.function, planned.value).intersectWith(pass.types.at(arm.testedType), pass.types), arm.builtinClass).containsAll(planned.aspects);
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
    IterationPlan plan { .source = value };
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
    if (plan.arms.isEmpty() || restNeedsProtocol || !std::ranges::all_of(plan.arms, &IterationArm::namesAreProven))
        plan.arms.append(IterationArm { });
    return plan;
}

static bool hasCastWithTest(const Function& function, std::span<const ValueID> definitionAndCasts)
{
    return std::ranges::any_of(definitionAndCasts.subspan(1), [&](ValueID cast) { return !isCastWithoutTest(function.at(cast).opcode); });
}

static std::optional<BypassedIterator> findBypassedIterator(FunctionPassContext& pass, ValueID definition, BlockID blockOfOpen)
{
    const BuiltinTable& builtins = pass.program.builtins();
    const Value& call = pass.function.at(definition);
    if (call.opcode != Opcode::CallBuiltin || call.builtinCallForm() != BuiltinCallForm::Whole || !call.receiverOperand())
        return std::nullopt;
    const BuiltinImplementation& implementation = builtins.at(call.overload()).code.implementation;
    if (!implementation.inlineBuiltin)
        return std::nullopt;

    ValueID receiver = call.receiverOperand();
    const Type& receiverType = pass.program.typeOf(pass.function, receiver);
    const ClassPart* part = receiverType.isObject() ? receiverType.singleBuiltinClass() : nullptr;
    std::optional<IterationForm> form = part ? formOfClass(builtins, part->builtinClass, IteratorKind::Sync) : std::nullopt;
    InlineBuiltin makerOfIterators = form == IterationForm::Map ? InlineBuiltin::MapIterator : form == IterationForm::Set ? InlineBuiltin::SetIterator : InlineBuiltin::ArrayIterator;
    if (!form || form == IterationForm::Generator || *implementation.inlineBuiltin != makerOfIterators)
        return std::nullopt;
    // An open inside a loop with its call outside resumes the same iterator on every run: only the first run starts at the beginning.
    if (!pass.analyses.blockCycles().isInEveryCycleOf(pass.analyses.useLists().blockOf(definition), blockOfOpen))
        return std::nullopt;

    return BypassedIterator { receiver, IterationArm {
        .form = *form,
        .kind = static_cast<JSC::IterationKind>(implementation.inlineParameters[0]),
        .builtinClass = part->builtinClass,
        .typedArrayType = builtins.at(part->builtinClass).typedArrayType,
        .testedType = pass.function.at(receiver).type,
        .classIsProven = true,
        .namesAreProven = true,
        .bypassesIterator = true,
    } };
}

namespace {

struct MergedIterators {
    FunctionPassContext& pass;
    ValueID open;
    BlockID blockOfOpen;
    Arms classArms { };
    Vector<ArmSource, 2> sources { };
    Vector<BlockID, 2> sourcesOfOtherValues { };
    Vector<BlockID, 2> mergingBlocks { };
    // A test behind a merge runs whichever block control came from, so it has to find the iterator.
    bool isTestedAfterMerge { false };

    bool isSourceOrMergingBlock(BlockID block) const
    {
        return mergingBlocks.contains(block) || sourcesOfOtherValues.contains(block) || sources.containsIf([&](const ArmSource& source) { return source.block == block; });
    }

    // False where one block would have to stand for two edges.
    bool collectSourcesOfArgument(Edge edge, ValueID argument)
    {
        const Function& function = pass.function;
        const UseLists& useLists = pass.analyses.useLists();
        if (isSourceOrMergingBlock(edge.from))
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
            sourcesOfOtherValues.append(edge.from);
            return true;
        }

        ValueLocation location = useLists.locationOf(chain.first());
        if (location.placement == ValuePlacement::Parameter && location.block == edge.from && location.block != entryBlock) {
            isTestedAfterMerge |= hasCastWithTest(function, chain.span());
            return collectSourcesOfParameter(chain.first());
        }

        std::optional<BypassedIterator> bypassed = findBypassedIterator(pass, chain.first(), blockOfOpen);
        size_t arm = bypassed ? classArms.find(bypassed->arm) : notFound;
        if (bypassed && arm == notFound && !pass.exceeds(LimitID::MaxDispatchCases, classArms.size() + 1, open)) {
            arm = classArms.size();
            classArms.append(bypassed->arm);
        }
        if (arm != notFound)
            sources.append(ArmSource { edge.from, static_cast<unsigned>(arm), bypassed->receiver, use });
        else
            sourcesOfOtherValues.append(edge.from);
        return true;
    }

    bool collectSourcesOfParameter(ValueID parameter)
    {
        ValueLocation location = pass.analyses.useLists().locationOf(parameter);
        if (isSourceOrMergingBlock(location.block))
            return false;
        mergingBlocks.append(location.block);

        for (Edge edge : pass.analyses.cfg().incomingEdges(location.block)) {
            if (edge.isHandlerEdge())
                return false;
            const Arguments& arguments = targetOf(pass.function, edge).arguments;
            // What a test passes along its edge is no iterator that a call has just made.
            if (location.index >= arguments.size()) {
                if (isSourceOrMergingBlock(edge.from))
                    return false;
                sourcesOfOtherValues.append(edge.from);
            } else if (!collectSourcesOfArgument(edge, arguments[location.index]))
                return false;
        }
        return true;
    }
};

} // anonymous namespace

// A call under tests joins the call it stands in for: `m.values()` on a Map that nobody proved pristine makes its iterator in one block and calls whatever `values` holds in another.
static std::optional<IterationPlan> planIteratorsBypassedBeforeMerge(FunctionPassContext& pass, const Iteration& iteration, std::span<const ValueID> parameterAndCasts)
{
    MergedIterators merged { pass, iteration.open, pass.analyses.useLists().blockOf(iteration.open) };
    ValueLocation location = pass.analyses.useLists().locationOf(parameterAndCasts.front());
    if (location.placement != ValuePlacement::Parameter || location.block != merged.blockOfOpen || location.block == entryBlock)
        return std::nullopt;
    merged.isTestedAfterMerge = hasCastWithTest(pass.function, parameterAndCasts);
    if (!merged.collectSourcesOfParameter(parameterAndCasts.front()) || merged.sources.isEmpty())
        return std::nullopt;

    // The blocks in which the open is live are split and copied, and what is defined at the end of a source has to stay one definition.
    if (std::ranges::any_of(findLiveRegion(pass, iteration.open), [&](BlockID block) { return merged.isSourceOrMergingBlock(block); }))
        return std::nullopt;

    if (merged.isTestedAfterMerge) {
        for (ArmSource& source : merged.sources)
            source.passedIterator = std::nullopt;
    }

    IterationPlan plan { .source = pass.function.at(iteration.open).iterableOperand(), .arms = WTF::move(merged.classArms), .armSources = WTF::move(merged.sources) };
    if (!merged.sourcesOfOtherValues.isEmpty()) {
        unsigned protocolArm = plan.arms.size();
        plan.arms.append(IterationArm { });
        for (BlockID block : merged.sourcesOfOtherValues)
            plan.armSources.append(ArmSource { block, protocolArm, { }, std::nullopt });
    }
    return plan;
}

static bool allBetweenInOneBlock(FunctionPassContext& pass, ValueID call, ValueID open, NOESCAPE const Invocable<bool(ValueID)> auto& predicate)
{
    ValueLocation first = pass.analyses.useLists().locationOf(call);
    ValueLocation last = pass.analyses.useLists().locationOf(open);
    if (first.block != last.block)
        return false;
    const BlockValues& values = pass.function.at(first.block).values;
    for (uint32_t index = first.index + 1; index < last.index; ++index) {
        if (!predicate(values[index]))
            return false;
    }
    return true;
}

// A string never changes, so its substrings are the same whenever they are cut: each step searches for the next separator, and nobody needs the array.
static std::optional<IterationPlan> planSplitString(FunctionPassContext& pass, const Iteration& iteration, const Vector<ValueID, 2>& chain)
{
    const Value& open = pass.function.at(iteration.open);
    if (open.isDestructuring() && !open.hasRest())
        return std::nullopt;
    std::optional<SplitString> splitString = splitStringOf(pass, pass.function.at(chain.first()));
    // The first search throws what the call may throw.
    if (!splitString || !allBetweenInOneBlock(pass, chain.first(), iteration.open, [&](ValueID value) { return pass.analyses.isUnobservable(value); }))
        return std::nullopt;
    if (!hasConstantIteratorMethod(pass, IterationArm { .form = IterationForm::Array, .builtinClass = BuiltinClasses::Array }))
        return std::nullopt;

    IterationArm arm {
        .form = IterationForm::SplitString,
        .testedType = pass.function.at(splitString->string).type,
        .separator = splitString->separator,
        .separatorSearch = splitString->separatorSearch,
        .classIsProven = true,
        .namesAreProven = true,
    };
    return IterationPlan { .source = splitString->string, .arms = { arm }, .replacedValues = chain };
}

// Also a call that nobody has resolved yet: its callee is the function itself.
static BuiltinID findBuiltinCalledBy(const BuiltinTable& builtins, const Function& function, const Value& call)
{
    if (call.opcode == Opcode::Call) {
        const Value& callee = function.at(call.calleeOperand());
        return callee.opcode == Opcode::Builtin ? builtins.canonicalID(callee.builtin()) : BuiltinID();
    }
    if (call.opcode != Opcode::CallBuiltin || (call.builtinCallForm() != BuiltinCallForm::Whole && call.builtinCallForm() != BuiltinCallForm::AnyReceiver))
        return { };
    const BuiltinOverload& overload = builtins.at(call.overload());
    return overload.mode == BuiltinCallMode::Call ? builtins.canonicalID(overload.builtin) : BuiltinID();
}

// `for (const [key, value] of Object.entries(object))`: the loop alone has the list, so nobody sees how it holds the pairs.
static std::optional<IterationPlan> planOwnEntries(FunctionPassContext& pass, const Iteration& iteration, const Vector<ValueID, 2>& chain)
{
    const Function& function = pass.function;
    if (function.at(iteration.open).isDestructuring() || hasCastWithTest(function, chain.span()))
        return std::nullopt;
    const Value& call = function.at(chain.first());
    if (findBuiltinCalledBy(pass.program.builtins(), function, call) != Builtins::ObjectEntries || call.argumentsOperands().size() != 1)
        return std::nullopt;
    // The call moves down to the open. Code of the program may run in it, so what stands between sees and changes nothing.
    bool hasNothingBetween = allBetweenInOneBlock(pass, chain.first(), iteration.open, [&](ValueID value) {
        Effects effects = pass.analyses.effectsOf(value).onAnyEdge();
        return effects.isUnobservable() && effects.reads.containsOnly(bookkeepingHeaps) && effects.writes.containsOnly(bookkeepingHeaps);
    });
    if (!hasNothingBetween || !hasConstantIteratorMethod(pass, IterationArm { .form = IterationForm::Array, .builtinClass = BuiltinClasses::Array }))
        return std::nullopt;

    // Where the layouts of the object are known, ExpandOwnKeys reads its slots and calls nothing. That is the cheaper list.
    ValueID object = call.argumentsOperands()[0];
    ValueLocation location = pass.analyses.useLists().locationOf(chain.first());
    OwnKeyWalkSite site { .walk = OwnKeyWalk::Entries, .block = location.block, .index = location.index, .value = chain.first(), .source = object };
    UnchangedAllocations unchanged(pass, singleElementSpan(site));
    if (planOwnKeyWalk(pass, unchanged, site))
        return std::nullopt;
    return IterationPlan { .source = object, .arms = { IterationArm { .form = IterationForm::OwnEntries, .testedType = function.at(object).type, .classIsProven = true, .namesAreProven = true } }, .replacedValues = chain };
}

// A generator that nobody has changed gives itself for its iterator, so a loop over the object walks what the method makes.
static FunctionID findGeneratorMethodOpening(FunctionPassContext& pass, ValueID iterable)
{
    const Program& program = pass.program;
    PropertyAccessQuery query;
    query.key = program.keyOf(program.builtins().nameOf(WellKnownSymbol::Iterator));
    query.receivers = receiverCandidates(program, pass.function, iterable);
    query.isOnlyCalled = true;
    query.usesOnlyPlansWithoutTests = true;
    if (!query.receivers.isProven() || program.typeOf(pass.function, iterable).mayBeUndefinedOrNull())
        return { };
    PropertyAccessPlan plan = planPropertyAccess({ program, &pass.inputs.flow }, query);
    if (plan.form != AccessForm::Method || !plan.needsNoTest() || plan.capturesReceiver || !mayBeCalledDirectly(program, plan.member.method))
        return { };

    const Function& method = program.at(plan.member.method);
    if (method.results.size() != 1 || method.results[0].flags.contains(PositionFlags::IsExpected))
        return { };
    const Type& made = program.at(method.results[0].type);
    bool isUnchangedGenerator = made.isSubsetOf(Type::builtinClass(BuiltinClasses::Generator), program) && pristineAspectsProvenBy(made, BuiltinClasses::Generator).contains(PristineAspects::Names);
    return isUnchangedGenerator && hasConstantIteratorMethod(pass, IterationArm { .form = IterationForm::Generator, .builtinClass = BuiltinClasses::Generator }) ? plan.member.method : FunctionID();
}

static IterationPlan planIteration(FunctionPassContext& pass, const Iteration& iteration)
{
    const Function& function = pass.function;
    const UseLists& useLists = pass.analyses.useLists();
    bool isSentValues = std::ranges::any_of(iteration.resumes, [&](ValueID resume) { return !!function.at(resume).sentOperand(); });
    bool isOnlyAskedForNextValue = !isSentValues && std::ranges::all_of(iteration.resumes, [&](ValueID resume) { return function.at(resume).resumeMode() == ResumeMode::Next; });
    PlannedValue planned { function.at(iteration.open).iterableOperand(), useLists.blockOf(iteration.open), { PristineAspects::Names }, iteration.kind, isSentValues, isOnlyAskedForNextValue };
    if (iteration.kind == IteratorKind::Async)
        return planWithClassArms(pass, planned.value, findClassArms(pass, planned));

    if (Vector<ValueID, 2> chain = findSingleUseCastChain(function, useLists, iteration.open); !chain.isEmpty()) {
        std::optional<BypassedIterator> bypassed = findBypassedIterator(pass, chain.first(), planned.block);
        if (bypassed)
            return IterationPlan { .source = bypassed->receiver, .arms = { bypassed->arm } };
        if (std::optional<IterationPlan> merged = planIteratorsBypassedBeforeMerge(pass, iteration, chain.span()))
            return WTF::move(*merged);
        if (std::optional<IterationPlan> splitString = planSplitString(pass, iteration, chain))
            return WTF::move(*splitString);
        if (std::optional<IterationPlan> ownEntries = planOwnEntries(pass, iteration, chain))
            return WTF::move(*ownEntries);
    }

    // The protocol arm of a value that may be a string nobody expected walks the string itself.
    IterationPlan plan = planWithClassArms(pass, planned.value, findClassArms(pass, planned));
    if (isOnlyAskedForNextValue && plan.hasProtocolArm() && !plan.arms.containsIf([](const IterationArm& arm) { return arm.form == IterationForm::String; }))
        plan.arms.last().alsoServesStrings = canServe(pass, planned, IterationArm { .form = IterationForm::String, .testedType = TypeTable::string, .namesAreProven = true });
    return plan;
}

// Arms that walk by a position.

static Range positionRangeOf(IterationForm form)
{
    switch (form) {
    case IterationForm::Array:
    case IterationForm::TypedArray:
        return { 0, maxExactInteger - 1 };
    case IterationForm::OwnEntries:
        // A step adds two to an index whose type only says that it is below the length.
        return { 0, maxExactInteger };
    case IterationForm::String:
        // A surrogate pair adds two to an index whose type only says that it is below the length.
        return { 0, Range::length().maximum + 1 };
    case IterationForm::SplitString:
        return positionRangeOfSplitString();
    case IterationForm::Protocol:
    case IterationForm::Map:
    case IterationForm::Set:
    case IterationForm::Generator:
    case IterationForm::AsyncGenerator:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static TypeID positionTypeOf(TypeArena& types, IterationForm form)
{
    return types.intern(Type::integer(positionRangeOf(form)));
}

static void emitIndexedOpen(IterationExpansion& expansion, ArmState& state)
{
    IRBuilder& builder = expansion.builder;
    if (state.patternRead == PatternRead::ByIndex)
        return;
    // The run time lists the keys and values as Object.entries does, at the moment the call stood for, one behind the other in one list.
    if (state.arm.form == IterationForm::OwnEntries)
        state.subject = builder.append(makeBuiltinCall(*expansion.program.builtins().overloadIDsOf(Builtins::CompilerHelpersOwnEntriesFlat).begin(), BuiltinCallForm::Whole, { { }, { state.subject } }, { }));
    // The call that makes an iterator validates the view. The @@iterator method of that iterator validates nothing.
    if (state.arm.form == IterationForm::TypedArray && !state.arm.bypassesIterator)
        builder.append(Opcode::RequireAttachedTypedArray, { state.subject });

    // A step is typed from the definition it names, before SSARepair picks the one that reaches it: every definition lies in the type of this one.
    ValueID zero = builder.appendInteger(0);
    BlockID start = expansion.addSiblingBlock();
    ValueID position = builder.addParameter(start, positionTypeOf(expansion.types, state.arm.form));
    builder.emitJump(start, { zero });
    builder.moveToEndOf(start);
    state.position = expansion.addVariable(position);
}

static void emitElementStep(IterationExpansion& expansion, const ArmState& state)
{
    IRBuilder& builder = expansion.builder;
    bool isTypedArray = state.arm.form == IterationForm::TypedArray;
    if (isTypedArray)
        builder.append(Opcode::RequireAttachedTypedArray, { state.subject });
    ValueID length = isTypedArray ? builder.append(makeWith(Opcode::GetTypedArrayLength, { state.subject }, &Value::setTypedArrayType, state.arm.typedArrayType)) : expansion.appendLength(state.subject);
    IRBuilder::ProvenBranch branch = expansion.branchOnInRange(state, length);
    ValueID index = branch.provenOnTaken;

    ValueID element;
    if (state.arm.kind != JSC::IterationKind::Keys && isTypedArray) {
        Value read = makeWith(Opcode::GetTypedArrayElement, { state.subject, index }, &Value::setTypedArrayType, state.arm.typedArrayType);
        read.setKeyIsIntegral(true);
        element = builder.append(WTF::move(read));
    } else if (state.arm.kind != JSC::IterationKind::Keys)
        element = expansion.appendElement(state.subject, index);
    expansion.define(*state.position, expansion.appendIncrement(index, 1));
    ValueID done = builder.appendBoolean(false);
    expansion.finishStep(done, expansion.appendValueOfKind(state.arm.kind, index, element));
    expansion.finishStepAsDoneIn(branch.notTaken);
}

static void emitStringStep(IterationExpansion& expansion, const ArmState& state)
{
    IRBuilder& builder = expansion.builder;
    IRBuilder::ProvenBranch branch = expansion.branchOnInRange(state, builder.append(Opcode::GetStringLength, { state.subject }));
    ValueID index = branch.provenOnTaken;

    ValueID codePointAt = expansion.appendBuiltin(Builtins::StringPrototypeCodePointAt);
    ValueID codePoint = expansion.appendCall(codePointAt, state.subject, singleElementSpan(index));
    ValueID isAstral = builder.append(Opcode::Greater, { codePoint, builder.appendInteger(0xFFFF) });

    BlockID merge = expansion.addSiblingBlock();
    ValueID end = builder.addParameter(merge, positionTypeOf(expansion.types, state.arm.form));
    IRBuilder::BranchBlocks blocks = builder.emitBranch(isAstral, BlockFrequency::Unlikely, BlockFrequency::Normal);
    builder.emitJump(merge, { expansion.appendIncrement(index, 2) });
    builder.moveToEndOf(blocks.notTaken);
    builder.emitJump(merge, { expansion.appendIncrement(index, 1) });

    builder.moveToEndOf(merge);
    ValueID character = builder.append(Opcode::Substring, { state.subject, index, end });
    expansion.define(*state.position, end);
    expansion.finishStep(builder.appendBoolean(false), character);
    expansion.finishStepAsDoneIn(branch.notTaken);
}

static void emitSplitStringStep(IterationExpansion& expansion, const ArmState& state)
{
    IRBuilder& builder = expansion.builder;
    SplitStringStep step = appendSplitStringStep(builder, { state.subject, state.arm.separator, state.arm.separatorSearch }, state.position->anyDefinition);
    ValueID substring = builder.append(Opcode::Substring, { state.subject, step.start, step.end });
    expansion.define(*state.position, step.next);
    expansion.finishStep(builder.appendBoolean(false), substring);
    expansion.finishStepAsDoneIn(step.done);
}

// The pair is made here, where whoever takes it apart sees what it is made of.
static void emitOwnEntriesStep(IterationExpansion& expansion, const ArmState& state)
{
    IRBuilder::ProvenBranch branch = expansion.branchOnInRange(state, expansion.appendLength(state.subject));
    ValueID index = branch.provenOnTaken;
    ValueID key = expansion.appendElement(state.subject, index);
    ValueID value = expansion.appendElement(state.subject, expansion.appendIncrement(index, 1));
    expansion.define(*state.position, expansion.appendIncrement(index, 2));
    expansion.finishStep(expansion.builder.appendBoolean(false), expansion.appendPair(key, value));
    expansion.finishStepAsDoneIn(branch.notTaken);
}

// Maps and sets.

static_assert(static_cast<uint32_t>(SetNextResult::HasEntry) == static_cast<uint32_t>(MapNextResult::HasEntry) && static_cast<uint32_t>(SetNextResult::Entry) == static_cast<uint32_t>(MapNextResult::Entry) && static_cast<uint32_t>(SetNextResult::Element) == static_cast<uint32_t>(MapNextResult::Key));

// On the edge that has an entry, a result of the step is what the collection holds. A collection whose contents have the type never is always empty. Nothing may follow the value that says so.
static bool tryNarrowToContentsOfEntry(IterationExpansion& expansion, const ArmState& state, ValueID& result, ContentsSlot slot)
{
    TypingContext typing { expansion.program, expansion.function, expansion.types, TypeLevel::Proven };
    std::optional<Type> contents = tryTypeOfContentsRead(typing, typing.typeOf(state.subject), slot);
    if (contents && !typing.typeOf(result).isSubsetOf(*contents, expansion.program)) {
        TypeID castType = expansion.types.intern(*contents);
        if (expansion.pass->build.validates())
            expansion.builder.append(makeValidateType(result, castType));
        result = expansion.builder.append(makeAssumeType(result, castType));
    }
    return expansion.function.at(result).type != TypeTable::never;
}

static void emitCollectionStep(IterationExpansion& expansion, const ArmState& state)
{
    IRBuilder& builder = expansion.builder;
    bool isMap = state.arm.form == IterationForm::Map;
    RELEASE_ASSERT(isMap || state.arm.form == IterationForm::Set);
    ValueID step = builder.append(isMap ? Opcode::MapNext : Opcode::SetNext, { state.subject, state.table->anyDefinition, state.position->anyDefinition });
    ValueID hasEntry = expansion.appendResultAt(step, MapNextResult::HasEntry);
    ValueID entry = expansion.appendResultAt(step, MapNextResult::Entry);
    ValueID key = expansion.appendResultAt(step, MapNextResult::Key);
    ValueID value = isMap ? expansion.appendResultAt(step, MapNextResult::Value) : key;

    IRBuilder::BranchBlocks blocks = builder.emitBranch(hasEntry);
    if (tryNarrowToContentsOfEntry(expansion, state, key, isMap ? ContentsSlot::Key : ContentsSlot::Element) && (!isMap || tryNarrowToContentsOfEntry(expansion, state, value, ContentsSlot::Value))) {
        // The entry number counts in the table the collection has now: the step followed a replaced table to its successor.
        expansion.define(*state.table, builder.append(Opcode::CollectionTable, { state.subject }));
        expansion.define(*state.position, expansion.appendIncrement(entry, 1));
        ValueID done = builder.appendBoolean(false);
        expansion.finishStep(done, expansion.appendValueOfKind(state.arm.kind, key, isMap ? value : key));
    } else
        builder.emitUnreachable();
    expansion.finishStepAsDoneIn(blocks.notTaken);
}

// Iterators that are asked by their methods, and generators.

static void finishStepWithValueUnlessDone(IterationExpansion& expansion, ValueID done, bool readsValueWhenDone, NOESCAPE const Invocable<ValueID()> auto& appendValue)
{
    if (readsValueWhenDone) {
        expansion.finishStep(done, appendValue());
        return;
    }
    IRBuilder::BranchBlocks blocks = expansion.builder.emitBranch(done);
    expansion.finishStepAsDone();
    expansion.builder.moveToEndOf(blocks.notTaken);
    ValueID value = appendValue();
    expansion.finishStep(expansion.builder.appendBoolean(false), value);
}

static void emitIteratorResultReads(IterationExpansion& expansion, ValueID result, bool readsValueWhenDone)
{
    ValueID object = expansion.appendRequireObject(result, expansion.origin == IterationOrigin::Spread ? RuntimeErrorKind::SpreadResultNotObject : RuntimeErrorKind::IteratorResultNotObject);
    ValueID done = expansion.builder.append(Opcode::ToBoolean, { expansion.appendPropGet(object, BuiltinNames::done) });
    finishStepWithValueUnlessDone(expansion, done, readsValueWhenDone, [&] { return expansion.appendPropGet(object, BuiltinNames::value); });
}

// The step suspends the function by itself where it has to wait: nothing is awaited behind it.
static void finishAsyncIteratorStep(IterationExpansion& expansion, ValueID step, const StepRequest& request)
{
    ValueID value = expansion.appendResultAt(step, StepGeneratorResult::Value);
    ValueID status = expansion.appendResultAt(step, StepGeneratorResult::Status);
    IRBuilder::BranchBlocks blocks = expansion.builder.emitBranch(expansion.appendIsEqualToInteger(status, AsyncGeneratorStepStatus::Result), BlockFrequency::Rare, BlockFrequency::Normal);
    emitIteratorResultReads(expansion, value, request.readsValueWhenDone);

    expansion.builder.moveToEndOf(blocks.notTaken);
    finishStepWithValueUnlessDone(expansion, expansion.appendIsEqualToInteger(status, AsyncGeneratorStepStatus::Completed), request.readsValueWhenDone, [&] { return value; });
}

static ArmState stringStateOf(const ArmState& state, ValueID string)
{
    return ArmState { .arm = IterationArm { .form = IterationForm::String, .testedType = TypeTable::string, .classIsProven = true, .namesAreProven = true }, .subject = string, .position = state.position };
}

static ArmState withSubject(const ArmState& state, ValueID subject)
{
    ArmState narrowed = state;
    narrowed.subject = subject;
    return narrowed;
}

static ValueID appendRequireObjectCoercible(IterationExpansion& expansion, ValueID iterable)
{
    if (!expansion.program.typeOf(expansion.function, iterable).mayBeUndefinedOrNull())
        return iterable;
    return expansion.builder.append(makeRequireObjectCoercible(iterable, expansion.origin));
}

static ValueID appendCallOfSharedFunction(IterationExpansion& expansion, OutlinedIteration operation, std::span<const ValueID> arguments)
{
    DerivedFunctionKey key { .role = FunctionRole::OutlinedBody, .target = FunctionID(), .parameter = 0, .node = static_cast<uint32_t>(operation) };
    return appendDirectCall(expansion.builder, { .callee = expansion.pass->derivedFunction(key), .arguments = arguments, .isNeverInlined = true });
}

static bool isOpenedBySharedFunction(IterationExpansion& expansion, ValueID iterable)
{
    if (expansion.isMakingSharedFunction() || expansion.origin != IterationOrigin::Source || isOutlinedIteration(expansion.function))
        return false;
    return !expansion.program.typeOf(expansion.function, iterable).layouts;
}

enum class SyncIteratorUse : uint8_t { Stepped, WrappedForAsyncIteration };

static OpenedIterator openSyncIterator(IterationExpansion& expansion, ValueID iterable, SyncIteratorUse use = SyncIteratorUse::Stepped)
{
    IRBuilder& builder = expansion.builder;
    bool isWrapped = use == SyncIteratorUse::WrappedForAsyncIteration;
    if (!isWrapped && isOpenedBySharedFunction(expansion, iterable)) {
        ValueID opened = appendCallOfSharedFunction(expansion, OutlinedIteration::OpenIterator, singleElementSpan(iterable));
        return { expansion.appendResultAt(opened, 0), expansion.appendResultAt(opened, 1), { }, true };
    }

    iterable = appendRequireObjectCoercible(expansion, iterable);
    IRBuilder::ProvenBranch branch = expansion.branchOnType(expansion.appendPropGet(iterable, WellKnownSymbol::Iterator), TypeTable::function, BlockFrequency::Normal, BlockFrequency::Rare);

    builder.moveToEndOf(branch.notTaken);
    if (isWrapped)
        expansion.emitThrow(RuntimeErrorKind::NoSyncIteratorForAsyncIteration);
    else if (expansion.origin == IterationOrigin::Delegation) {
        expansion.appendCall(branch.provenOnNotTaken, iterable, { });
        expansion.emitThrow(RuntimeErrorKind::IteratorIsNotAnObject);
    } else {
        builder.append(makeWith(Opcode::ThrowNotIterable, { iterable }, &Value::setIsSpread, expansion.origin == IterationOrigin::Spread));
        builder.emitUnreachable();
    }

    builder.moveToEndOf(branch.taken);
    if (!isWrapped && expansion.origin != IterationOrigin::Source) {
        // The engine's spread and `yield*` read `next` of whatever the method returns, and call it.
        IRBuilder::ProvenBranch made = expansion.branchOnType(expansion.appendCall(branch.provenOnTaken, iterable, { }), TypeTable::object, BlockFrequency::Normal, BlockFrequency::Rare);
        builder.moveToEndOf(made.notTaken);
        expansion.appendCall(expansion.appendPropGet(made.provenOnNotTaken, BuiltinNames::next), made.provenOnNotTaken, { });
        expansion.emitThrow(RuntimeErrorKind::IteratorIsNotAnObject);
        builder.moveToEndOf(made.taken);
        return { made.provenOnTaken, expansion.appendPropGet(made.provenOnTaken, BuiltinNames::next), { } };
    }
    ValueID iterator = expansion.appendIteratorMethodCall(branch.provenOnTaken, iterable, isWrapped ? RuntimeErrorKind::SyncIteratorForAsyncIterationIsNotAnObject : RuntimeErrorKind::IteratorIsNotAnObject);
    return { iterator, expansion.appendPropGet(iterator, BuiltinNames::next), { } };
}

static ValueID openWrappedIterator(IterationExpansion& expansion, ValueID iterable)
{
    if (isOpenedBySharedFunction(expansion, iterable))
        return appendCallOfSharedFunction(expansion, OutlinedIteration::OpenWrappedIterator, singleElementSpan(iterable));
    OpenedIterator sync = openSyncIterator(expansion, iterable, SyncIteratorUse::WrappedForAsyncIteration);
    return expansion.builder.append(Opcode::CreateAsyncFromSyncIterator, { sync.iterator, sync.next });
}

static OpenedIterator openAsyncIterator(IterationExpansion& expansion, ValueID iterable)
{
    IRBuilder& builder = expansion.builder;
    iterable = appendRequireObjectCoercible(expansion, iterable);
    ValueID method = expansion.appendPropGet(iterable, WellKnownSymbol::AsyncIterator);
    BlockID merge = expansion.addSiblingBlock();
    ValueID iterator = builder.addParameter(merge, TypeTable::object);
    ValueID isFromSyncIterator = builder.addParameter(merge, TypeTable::boolean);
    IRBuilder::ProvenBranch branch = expansion.branchOnType(method, TypeTable::nullish);

    ValueID wrapper = openWrappedIterator(expansion, iterable);
    builder.emitJump(merge, { wrapper, builder.appendBoolean(true) });

    builder.moveToEndOf(branch.notTaken);
    ValueID made = expansion.appendIteratorMethodCall(branch.provenOnNotTaken, iterable);
    builder.emitJump(merge, { made, builder.appendBoolean(false) });

    builder.moveToEndOf(merge);
    ValueID next = expansion.appendPropGet(iterator, BuiltinNames::next);
    BlockID opened = expansion.addSiblingBlock();
    ValueID isSteppedWithoutCall = builder.addParameter(opened, TypeTable::boolean);
    IRBuilder::BranchBlocks fromSyncIterator = builder.emitBranch(isFromSyncIterator);
    builder.emitJump(opened, { builder.appendBoolean(true) });

    builder.moveToEndOf(fromSyncIterator.notTaken);
    IRBuilder::BranchBlocks asyncGenerator = builder.emitBranch(builder.appendTypeTest(iterator, Type::builtinClass(BuiltinClasses::AsyncGenerator)));
    builder.emitJump(opened, { builder.appendTypeTest(next, Type::forBuiltin(Builtins::AsyncGeneratorPrototypeNext)) });

    builder.moveToEndOf(asyncGenerator.notTaken);
    builder.emitJump(opened, { builder.appendBoolean(false) });

    builder.moveToEndOf(opened);
    return { iterator, next, isSteppedWithoutCall };
}

static void emitProtocolOpen(IterationExpansion& expansion, ArmState& state, IteratorKind kind)
{
    IRBuilder& builder = expansion.builder;
    if (!state.arm.alsoServesStrings) {
        OpenedIterator opened = kind == IteratorKind::Async ? openAsyncIterator(expansion, state.subject) : openSyncIterator(expansion, state.subject);
        state.subject = opened.iterator;
        state.nextMethod = opened.next;
        state.isSteppedWithoutCall = opened.isSteppedWithoutCall;
        state.callsSharedFunctions = opened.isOpenedBySharedFunction;
        return;
    }

    // From here on the subject is the string itself or the iterator of anything else, and every operation asks which.
    RELEASE_ASSERT(kind == IteratorKind::Sync);
    ArmState walked = stringStateOf(state, state.subject);
    emitIndexedOpen(expansion, walked);
    state.position = walked.position;

    if (isOpenedBySharedFunction(expansion, state.subject) && !expansion.isExpectedToRun()) {
        ValueID opened = appendCallOfSharedFunction(expansion, OutlinedIteration::OpenStringOrIterator, singleElementSpan(state.subject));
        state.subject = expansion.appendResultAt(opened, 0);
        state.nextMethod = expansion.appendResultAt(opened, 1);
        state.callsSharedFunctions = true;
        return;
    }

    BlockID merge = expansion.addSiblingBlock();
    ValueID subject = builder.addParameter(merge, expansion.types.unionOf(TypeTable::object, TypeTable::string));
    IRBuilder::ProvenBranch branch = expansion.branchOnType(state.subject, TypeTable::string);
    ValueID nextOfString = builder.appendUndefined();
    builder.emitJump(merge, { branch.provenOnTaken });

    builder.moveToEndOf(branch.notTaken);
    OpenedIterator opened = openSyncIterator(expansion, branch.provenOnNotTaken);
    builder.emitJump(merge, { opened.iterator });

    // For a second parameter the builder looks for the edges into the block, which computes the control flow graph of the whole function at every open.
    expansion.define(expansion.addVariable(opened.next), nextOfString);

    builder.moveToEndOf(merge);
    state.subject = subject;
    state.nextMethod = opened.next;
    state.callsSharedFunctions = opened.isOpenedBySharedFunction;
}

static void emitCloseCalls(IterationExpansion& expansion, const ArmState& state, IteratorKind kind, bool checksResult, BlockID after)
{
    IRBuilder& builder = expansion.builder;
    IRBuilder::ProvenBranch branch = expansion.branchOnType(expansion.appendPropGet(state.subject, BuiltinNames::returnKeyword), TypeTable::nullish);
    builder.emitJump(after);

    builder.moveToEndOf(branch.notTaken);
    ValueID result = expansion.appendAwaitIfAsync(kind, expansion.appendCall(branch.provenOnNotTaken, state.subject, { }));
    if (checksResult)
        expansion.appendRequireObject(result, RuntimeErrorKind::IteratorResultNotObject);
    builder.emitJump(after);
}

static void emitCloseOfIterator(IterationExpansion& expansion, const ArmState& state, IteratorKind kind, IteratorCompletion completion, BlockID after)
{
    IRBuilder& builder = expansion.builder;
    if (state.callsSharedFunctions && kind == IteratorKind::Sync) {
        appendCallOfSharedFunction(expansion, completion == IteratorCompletion::Normal ? OutlinedIteration::CloseIterator : OutlinedIteration::CloseIteratorAfterThrow, singleElementSpan(state.subject));
        builder.emitJump(after);
        return;
    }
    if (completion == IteratorCompletion::Normal) {
        emitCloseCalls(expansion, state, kind, true, after);
        return;
    }

    // What the calls throw is dropped.
    std::optional<Target> outer = builder.handler();
    BlockID dropped = builder.addHandlerBlock(BlockRole::CatchHandler, ValueID());
    builder.setHandler(Target { dropped, { }, { } });
    BlockID attempt = builder.addBlock(BlockFrequency::Rare);
    builder.emitJump(attempt);

    builder.moveToEndOf(attempt);
    emitCloseCalls(expansion, state, kind, false, after);
    builder.setHandler(WTF::move(outer));

    builder.moveToEndOf(dropped);
    builder.append(Opcode::CatchValue, { });
    builder.emitJump(after);
}

static void emitProtocolClose(IterationExpansion& expansion, const ArmState& state, IteratorKind kind, IteratorCompletion completion)
{
    IRBuilder& builder = expansion.builder;
    if (state.arm.alsoServesStrings && state.callsSharedFunctions && kind == IteratorKind::Sync && !expansion.isExpectedToRun()) {
        appendCallOfSharedFunction(expansion, completion == IteratorCompletion::Normal ? OutlinedIteration::CloseStringOrIterator : OutlinedIteration::CloseStringOrIteratorAfterThrow, singleElementSpan(state.subject));
        expansion.finishClose();
        return;
    }

    BlockID after = expansion.addSiblingBlock();
    if (state.arm.alsoServesStrings) {
        IRBuilder::ProvenBranch branch = expansion.branchOnType(state.subject, TypeTable::string);
        builder.emitJump(after);
        builder.moveToEndOf(branch.notTaken);
        emitCloseOfIterator(expansion, withSubject(state, branch.provenOnNotTaken), kind, completion, after);
    } else
        emitCloseOfIterator(expansion, state, kind, completion, after);

    builder.moveToEndOf(after);
    expansion.finishClose();
}

static ValueID appendResumeCall(IterationExpansion& expansion, const ArmState& state, const StepRequest& request)
{
    IRBuilder& builder = expansion.builder;
    if (request.mode == ResumeMode::Next)
        return expansion.appendCall(state.nextMethod, state.subject, request.sent ? singleElementSpan(request.sent) : std::span<const ValueID> { });

    bool isThrow = request.mode == ResumeMode::Throw;
    ValueID sent = request.sent ? request.sent : builder.appendUndefined();
    ValueID method = expansion.appendPropGet(state.subject, isThrow ? BuiltinNames::throwKeyword : BuiltinNames::returnKeyword);
    IRBuilder::ProvenBranch branch = expansion.branchOnType(method, TypeTable::nullish, isThrow ? BlockFrequency::Rare : BlockFrequency::Normal);
    if (isThrow) {
        BlockID closed = expansion.addSiblingBlock();
        emitCloseCalls(expansion, state, request.kind, true, closed);
        builder.moveToEndOf(closed);
        expansion.emitThrowOfMissingThrowMethod();
    } else {
        ValueID returned = expansion.appendAwaitIfAsync(request.kind, sent);
        expansion.finishStep(builder.appendBoolean(true), returned);
    }

    builder.moveToEndOf(branch.notTaken);
    return expansion.appendCall(branch.provenOnNotTaken, state.subject, singleElementSpan(sent));
}

static void emitStepByCall(IterationExpansion& expansion, const ArmState& state, const StepRequest& request)
{
    emitIteratorResultReads(expansion, expansion.appendAwaitIfAsync(request.kind, appendResumeCall(expansion, state, request)), request.readsValueWhenDone);
}

static void emitProtocolStep(IterationExpansion& expansion, const ArmState& state, const StepRequest& request)
{
    IRBuilder& builder = expansion.builder;
    if (state.arm.alsoServesStrings) {
        RELEASE_ASSERT(request.mode == ResumeMode::Next && !request.sent);
        bool walksStringBySharedFunction = state.callsSharedFunctions && !expansion.isExpectedToRun();
        IRBuilder::ProvenBranch branch = expansion.branchOnType(state.subject, TypeTable::string);
        if (walksStringBySharedFunction) {
            ValueID step = appendCallOfSharedFunction(expansion, OutlinedIteration::StepString, std::array { branch.provenOnTaken, state.position->anyDefinition });
            ValueID done = expansion.appendResultAt(step, doneResult);
            ValueID value = expansion.appendResultAt(step, valueResult);
            expansion.define(*state.position, expansion.appendResultAt(step, 2));
            expansion.finishStep(done, value);
        } else
            emitStringStep(expansion, stringStateOf(state, branch.provenOnTaken));

        builder.moveToEndOf(branch.notTaken);
        emitStepByCall(expansion, withSubject(state, branch.provenOnNotTaken), request);
        return;
    }

    // The program never sees the wrapper around a sync iterator, so nobody can tell that its `next` is not called. Where the method that was read from an async generator is the built-in `next`, the call does what the step does.
    if (request.kind == IteratorKind::Async && request.mode == ResumeMode::Next && state.isSteppedWithoutCall) {
        IRBuilder::BranchBlocks blocks = builder.emitBranch(state.isSteppedWithoutCall);
        // %AsyncFromSyncIteratorPrototype%.next passes on what it was called with: where nothing is sent, the iterator's `next` is called without an argument.
        ValueID sent = request.sent ? request.sent : builder.append(Opcode::Absent, { });
        finishAsyncIteratorStep(expansion, appendAsyncIteratorStep(builder, state.subject, sent), request);
        builder.moveToEndOf(blocks.notTaken);
    }
    emitStepByCall(expansion, state, request);
}

static ArmState protocolStateOf(IterationExpansion& expansion, const ArmState& state)
{
    BuiltinID next = state.arm.form == IterationForm::AsyncGenerator ? Builtins::AsyncGeneratorPrototypeNext : Builtins::GeneratorPrototypeNext;
    return ArmState { .arm = IterationArm { }, .subject = state.subject, .nextMethod = expansion.appendBuiltin(next) };
}

static ValueID appendStepGenerator(IterationExpansion& expansion, const ArmState& state, ResumeMode mode, ValueID sent)
{
    Value step = makeWith(Opcode::StepGenerator, { state.subject, sent }, &Value::setResumeMode, mode);
    step.setCheck(mode == ResumeMode::Next || state.arm.namesAreProven ? GeneratorStepCheck::None : GeneratorStepCheck::IsUnmodified);
    return expansion.builder.append(WTF::move(step));
}

static void emitGeneratorStep(IterationExpansion& expansion, const ArmState& state, const StepRequest& request)
{
    ValueID step = appendStepGenerator(expansion, state, request.mode, request.sent ? request.sent : expansion.builder.appendUndefined());
    ValueID value = expansion.appendResultAt(step, StepGeneratorResult::Value);
    ValueID status = expansion.appendResultAt(step, StepGeneratorResult::Status);
    IRBuilder::BranchBlocks blocks = expansion.branchOnNotStepped(status);
    emitProtocolStep(expansion, protocolStateOf(expansion, state), request);

    expansion.builder.moveToEndOf(blocks.notTaken);
    finishStepWithValueUnlessDone(expansion, expansion.appendIsEqualToInteger(status, GeneratorStepStatus::Completed), request.readsValueWhenDone, [&] { return value; });
}

static void emitAsyncGeneratorStep(IterationExpansion& expansion, const ArmState& state, const StepRequest& request)
{
    if (request.mode != ResumeMode::Next) {
        emitProtocolStep(expansion, protocolStateOf(expansion, state), request);
        return;
    }
    ValueID sent = request.sent ? request.sent : expansion.builder.appendUndefined();
    finishAsyncIteratorStep(expansion, appendAsyncIteratorStep(expansion.builder, state.subject, sent), request);
}

static void emitGeneratorClose(IterationExpansion& expansion, const ArmState& state, IteratorCompletion completion)
{
    IRBuilder& builder = expansion.builder;
    BlockID dropped;
    if (completion == IteratorCompletion::Throw) {
        std::optional<Target> outer = builder.handler();
        dropped = builder.addHandlerBlock(BlockRole::CatchHandler, ValueID());
        builder.setHandler(Target { dropped, { }, { } });
        BlockID attempt = builder.addBlock(BlockFrequency::Rare);
        builder.setHandler(outer);
        builder.emitJump(attempt);
        builder.moveToEndOf(attempt);
    }

    ValueID step = appendStepGenerator(expansion, state, ResumeMode::Return, builder.appendUndefined());
    IRBuilder::BranchBlocks blocks = expansion.branchOnNotStepped(expansion.appendResultAt(step, StepGeneratorResult::Status));
    emitProtocolClose(expansion, protocolStateOf(expansion, state), IteratorKind::Sync, completion);
    builder.moveToEndOf(blocks.notTaken);
    expansion.finishClose();

    if (dropped) {
        builder.moveToEndOf(dropped);
        builder.append(Opcode::CatchValue, { });
        expansion.finishClose();
    }
}

// The operations of an arm.

static void emitArmStep(IterationExpansion& expansion, const ArmState& state, const StepRequest& request)
{
    if (!state.arm.hasMethodsToCall() && request.mode == ResumeMode::Throw) {
        expansion.emitThrowOfMissingThrowMethod();
        return;
    }
    if (!state.arm.hasMethodsToCall() && request.mode == ResumeMode::Return) {
        ValueID value = request.sent ? request.sent : expansion.builder.appendUndefined();
        expansion.finishStep(expansion.builder.appendBoolean(true), value);
        return;
    }

    switch (state.arm.form) {
    case IterationForm::Array:
    case IterationForm::TypedArray:
        emitElementStep(expansion, state);
        return;
    case IterationForm::String:
        emitStringStep(expansion, state);
        return;
    case IterationForm::SplitString:
        emitSplitStringStep(expansion, state);
        return;
    case IterationForm::OwnEntries:
        emitOwnEntriesStep(expansion, state);
        return;
    case IterationForm::Map:
    case IterationForm::Set:
        emitCollectionStep(expansion, state);
        return;
    case IterationForm::Generator:
        emitGeneratorStep(expansion, state, request);
        return;
    case IterationForm::AsyncGenerator:
        emitAsyncGeneratorStep(expansion, state, request);
        return;
    case IterationForm::Protocol:
        emitProtocolStep(expansion, state, request);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Functions that many iterations share.

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

static Function buildSharedFunction(DerivedFunctionInputs& inputs, ASCIILiteral name, std::initializer_list<TypeID> parameterTypes, std::initializer_list<TypeID> resultTypes, NOESCAPE const Invocable<void(IterationExpansion&, std::span<const ValueID>)> auto& emitBody)
{
    Function function;
    function.name = inputs.strings.intern(StringView(name));
    function.kind = FunctionKind::Plain;
    function.role = FunctionRole::OutlinedBody;
    function.length = static_cast<uint32_t>(parameterTypes.size());
    function.thisType = DeclaredType { .type = TypeTable::anything };
    for (TypeID type : parameterTypes) {
        function.parameters.append(DeclaredType { .type = type });
        function.derivation.parameterMap.append(ParameterOrigin { });
    }
    for (TypeID type : resultTypes)
        function.results.append(DeclaredType { .type = type });
    function.capabilities = initialCapabilitiesOf(function);

    IRBuilder builder({ inputs.build, inputs.program, &inputs.flow, nullptr }, function, inputs.types);
    BlockID entry = builder.addBlock();
    Vector<ValueID, 2> parameters;
    for (TypeID type : parameterTypes)
        parameters.append(builder.addParameter(entry, type));
    builder.moveToEndOf(entry);
    IterationExpansion expansion { nullptr, inputs.program, function, inputs.types, builder, nullptr };
    emitBody(expansion, parameters.span());
    return function;
}

Function buildOutlinedIteration(const DerivedFunctionKey& key, DerivedFunctionInputs& inputs)
{
    using Parameters = std::span<const ValueID>;
    auto returnOpened = [](IterationExpansion& expansion, ValueID iterable) {
        OpenedIterator opened = openSyncIterator(expansion, iterable);
        expansion.builder.emitReturn(expansion.builder.append(Opcode::Results, { opened.iterator, opened.next }));
    };
    auto buildClose = [&](ASCIILiteral name, TypeID subjectType, IteratorCompletion completion) {
        return buildSharedFunction(inputs, name, { subjectType }, { TypeTable::undefined }, [&](IterationExpansion& expansion, Parameters parameters) {
            emitProtocolClose(expansion, ArmState { .arm = IterationArm { .alsoServesStrings = subjectType != TypeTable::object }, .subject = parameters[0] }, IteratorKind::Sync, completion);
        });
    };

    switch (static_cast<OutlinedIteration>(key.node)) {
    case OutlinedIteration::AppendIterable:
        return buildSharedFunction(inputs, "appendIterable"_s, { inputs.types.intern(Type::builtinClass(BuiltinClasses::Array)), TypeTable::anything }, { TypeTable::undefined }, [&](IterationExpansion& expansion, Parameters parameters) {
            expansion.function.edit(parameters[1], ValueEdit::Type).setExpected(inputs.types.intern(iterablesExpectedBy(key, inputs.types)));
            expansion.builder.append(Opcode::AppendSpread, { parameters[0], parameters[1] });
            expansion.finishClose();
        });
    case OutlinedIteration::OpenIterator:
        return buildSharedFunction(inputs, "openIterator"_s, { TypeTable::anything }, { TypeTable::object, TypeTable::anything }, [&](IterationExpansion& expansion, Parameters parameters) {
            returnOpened(expansion, parameters[0]);
        });
    case OutlinedIteration::CloseIterator:
        return buildClose("closeIterator"_s, TypeTable::object, IteratorCompletion::Normal);
    case OutlinedIteration::CloseIteratorAfterThrow:
        return buildClose("closeIteratorAfterThrow"_s, TypeTable::object, IteratorCompletion::Throw);
    case OutlinedIteration::OpenWrappedIterator:
        return buildSharedFunction(inputs, "openWrappedIterator"_s, { TypeTable::anything }, { TypeTable::object }, [&](IterationExpansion& expansion, Parameters parameters) {
            expansion.builder.emitReturn(openWrappedIterator(expansion, parameters[0]));
        });
    case OutlinedIteration::OpenStringOrIterator:
        return buildSharedFunction(inputs, "openStringOrIterator"_s, { TypeTable::anything }, { inputs.types.unionOf(TypeTable::object, TypeTable::string), TypeTable::anything }, [&](IterationExpansion& expansion, Parameters parameters) {
            IRBuilder::ProvenBranch branch = expansion.branchOnType(parameters[0], TypeTable::string);
            expansion.builder.emitReturn(expansion.builder.append(Opcode::Results, { branch.provenOnTaken, expansion.builder.appendUndefined() }));
            expansion.builder.moveToEndOf(branch.notTaken);
            returnOpened(expansion, branch.provenOnNotTaken);
        });
    case OutlinedIteration::StepString: {
        TypeID positionType = positionTypeOf(inputs.types, IterationForm::String);
        return buildSharedFunction(inputs, "stepString"_s, { TypeTable::string, positionType }, { TypeTable::boolean, TypeTable::anything, positionType }, [&](IterationExpansion& expansion, Parameters parameters) {
            expansion.positionInFrontOfStep = expansion.positionBehindStep = parameters[1];
            emitStringStep(expansion, stringStateOf(ArmState { .position = StateVariable { 0, parameters[1] } }, parameters[0]));
        });
    }
    case OutlinedIteration::CloseStringOrIterator:
        return buildClose("closeStringOrIterator"_s, inputs.types.unionOf(TypeTable::object, TypeTable::string), IteratorCompletion::Normal);
    case OutlinedIteration::CloseStringOrIteratorAfterThrow:
        return buildClose("closeStringOrIteratorAfterThrow"_s, inputs.types.unionOf(TypeTable::object, TypeTable::string), IteratorCompletion::Throw);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// %ArrayIteratorPrototype%.next on the state that JSArrayIterator keeps. Nobody knows what the iterator is over, so every read is the generic one.
static void emitArrayIteratorStep(IterationExpansion& expansion, ValueID iterator)
{
    IRBuilder& builder = expansion.builder;
    auto appendFieldRead = [&](JSArrayIterator::Field field) { return builder.append(makeWith(Opcode::GetInternalField, { iterator }, &Value::setIndex, static_cast<uint32_t>(field))); };
    auto appendPositionStore = [&](ValueID position) { builder.append(makeWith(Opcode::PutInternalField, { iterator, position }, &Value::setIndex, static_cast<uint32_t>(JSArrayIterator::Field::Index))); };
    auto finishWith = [&](ValueID value) { expansion.finishStep(builder.appendBoolean(false), value); };

    ValueID position = appendFieldRead(JSArrayIterator::Field::Index);
    IRBuilder::BranchBlocks finished = builder.emitBranch(expansion.appendIsEqualToInteger(position, JSArrayIterator::doneIndex));
    expansion.finishStepAsDone();

    // An array iterator is made for an object and keeps it.
    builder.moveToEndOf(finished.notTaken);
    ValueID iterated = convertTo(builder, appendFieldRead(JSArrayIterator::Field::IteratedObject), DeclaredType { .type = TypeTable::object });
    BlockID merge = expansion.addSiblingBlock();
    ValueID length = builder.addParameter(merge, TypeTable::number);
    Value isTypedArrayView = makeWith(Opcode::HasJSType, { iterated }, &Value::setFirst, CheckedJSType::Int8ArrayType);
    isTypedArrayView.setLast(CheckedJSType::BigUint64ArrayType);
    IRBuilder::BranchBlocks typedArrayView = builder.emitBranch(builder.append(WTF::move(isTypedArrayView)));
    builder.append(Opcode::RequireAttachedTypedArray, { iterated });
    builder.emitJump(merge, { builder.append(Opcode::GetTypedArrayLength, { iterated }) });
    builder.moveToEndOf(typedArrayView.notTaken);
    builder.emitJump(merge, { expansion.appendLength(iterated) });

    builder.moveToEndOf(merge);
    IRBuilder::BranchBlocks inRange = builder.emitBranch(builder.append(Opcode::Less, { position, length }));
    appendPositionStore(expansion.appendIncrement(position, 1));
    ValueID kind = appendFieldRead(JSArrayIterator::Field::Kind);
    IRBuilder::BranchBlocks keys = builder.emitBranch(expansion.appendIsEqualToInteger(kind, JSC::IterationKind::Keys));
    finishWith(position);

    builder.moveToEndOf(keys.notTaken);
    ValueID element = builder.append(Opcode::ElemGet, { iterated, position });
    IRBuilder::BranchBlocks entries = builder.emitBranch(expansion.appendIsEqualToInteger(kind, JSC::IterationKind::Entries));
    finishWith(expansion.appendPair(position, element));
    builder.moveToEndOf(entries.notTaken);
    finishWith(element);

    builder.moveToEndOf(inRange.notTaken);
    appendPositionStore(builder.appendInteger(JSArrayIterator::doneIndex));
    expansion.finishStepAsDone();
}

static PassOutcome expandArrayIteratorSteps(FunctionPassContext& pass, IRBuilder& builder)
{
    Vector<ProgramPoint, 1> steps;
    Vector<ValueID, 1> producers;
    forEachValueSite(pass.analyses, pass.function, { Opcode::StepArrayIterator }, [&](const ValueSite& site) {
        steps.append(ProgramPoint { site.block, site.index });
        producers.append(site.value);
    });
    if (steps.isEmpty())
        return PassOutcome::Unchanged;

    AOT::sort(steps, std::ranges::greater { }, blockAndIndexOf);
    SSARepair repair(builder, pass.analyses);
    IterationExpansion expansion { &pass, pass.program, pass.function, pass.types, builder, &repair };
    IRBuilder::DeferredResultReads resultReads(builder, producers.span());
    for (const ProgramPoint& point : steps) {
        Value original = pass.function.at(pass.function.at(point.block).values[point.index]);
        builder.setOrigin(original.origin());
        IRBuilder::Expansion cut = builder.beginExpansion(point, resultReads);
        expansion.operation = &cut;
        emitArrayIteratorStep(expansion, original.iteratorOperand());
    }
    resultReads.finish();
    builder.setHandler(std::nullopt);
    return PassOutcome::Changed;
}

static PassOutcome threadForInPositions(FunctionPassContext& pass, IRBuilder& builder)
{
    Function& function = pass.function;
    Vector<ValueSite, 2> opens;
    Vector<ValueSite, 4> unthreadedSteps;
    HashMap<ValueID, StateVariable> positions;
    forEachValueSite(pass.analyses, function, { Opcode::ForInOpen, Opcode::ForInStep }, [&](const ValueSite& site) {
        const Value& value = function.at(site.value);
        if (value.opcode == Opcode::ForInOpen)
            opens.append(site);
        else if (!value.positionOperand()) {
            unthreadedSteps.append(site);
            positions.add(value.enumeratorOperand(), StateVariable { });
        }
    });
    if (unthreadedSteps.isEmpty())
        return PassOutcome::Unchanged;

    SSARepair repair(builder, pass.analyses);
    for (const ValueSite& open : opens) {
        auto position = positions.find(open.value);
        if (position == positions.end())
            continue;
        builder.setOrigin(function.at(open.value).origin());
        ValueID zero = builder.insertBefore({ open.block, open.index + 1 }, makeInteger(0));
        position->value = StateVariable { repair.addVariable(zero), zero };
    }
    for (const ValueSite& step : unthreadedSteps) {
        StateVariable position = positions.get(function.at(step.value).enumeratorOperand());
        RELEASE_ASSERT(position.anyDefinition);
        ProgramPoint pointAfterStep { step.block, step.index + 1 };
        builder.setOrigin(function.at(step.value).origin());
        ValueID found = builder.insertBefore(pointAfterStep, makeResultAt(step.value, static_cast<uint32_t>(ForInStepResult::Position)));
        ValueID one = builder.insertBefore(pointAfterStep, makeInteger(1));
        ValueID next = builder.insertBefore(pointAfterStep, IRBuilder::make(Opcode::Add, { found, one }));
        function.edit(step.value, ValueEdit::Definition).setPositionOperand(position.anyDefinition);
        repair.addDefinition(position.variable, next);
    }

    builder.applyInsertions();
    pass.analyses.didChangeValues();
    repair.run();
    return PassOutcome::Changed;
}

// Destructuring patterns.

static bool stepsMatchElements(const Function& function, const Iteration& iteration, uint32_t count)
{
    BitVector steppedIndexes(count);
    for (ValueID id : iteration.resumes) {
        const Value& resume = function.at(id);
        std::optional<uint32_t> index = resume.stepIndex();
        if (resume.resumeMode() != ResumeMode::Next || resume.sentOperand() || resume.readsValueWhenDone() || !index || *index >= count || steppedIndexes.set(*index))
            return false;
    }
    return steppedIndexes.bitCount() == count;
}

static ValueID findFirstStep(const Function& function, const Iteration& iteration)
{
    size_t first = iteration.resumes.findIf([&](ValueID resume) { return function.at(resume).stepIndex() == 0u; });
    RELEASE_ASSERT(first != notFound);
    return iteration.resumes[first];
}

static bool isLandingCode(const Dominators& dominators, const Iteration& iteration, BlockID block)
{
    return iteration.cleanupBlocks.containsIf([&](BlockID cleanupBlock) { return dominators.dominates(cleanupBlock, block); });
}

// What may run between the open and the last operation of the pattern, but for those operations themselves and the landing code.
static bool anyOperationBetweenSteps(FunctionPassContext& pass, const Iteration& iteration, std::span<const BlockID> liveRegion, NOESCAPE const Invocable<bool(ValueID)> auto& matchesValue, NOESCAPE const Invocable<bool(BlockID)> auto& matchesTerminator)
{
    const Function& function = pass.function;
    auto isOperation = [&](ValueID id) {
        const Value& value = function.at(id);
        return (value.opcode == Opcode::IteratorResume || value.opcode == Opcode::IteratorClose) && value.iteratorOperand() == iteration.open;
    };
    BitVector liveAtHead = blockSetOf(function, liveRegion);
    for (BlockID id : liveRegion) {
        if (isLandingCode(pass.analyses.dominators(), iteration, id))
            continue;

        // The block of the last close goes on with the code after the pattern, which no step moves across.
        const BlockValues& values = function.at(id).values;
        bool isLiveAtTail = std::ranges::any_of(pass.analyses.cfg().successors(id), [&](BlockID successor) { return liveAtHead.get(successor.index()); });
        static_assert(!(notFound + 1));
        for (ValueID value : values.span().first(isLiveAtTail ? values.size() : values.reverseFindIf(isOperation) + 1)) {
            if (!isOperation(value) && matchesValue(value))
                return true;
        }
        if (isLiveAtTail && matchesTerminator(id))
            return true;
    }
    return false;
}

// The reads stay where the steps stood, so what stands between them only has to leave alone what they rely on: the length, and what the open tested.
static bool regionMayChangeArray(FunctionPassContext& pass, const Iteration& iteration, const IterationArm& arm, std::span<const BlockID> liveRegion, ValueID array)
{
    FunctionAnalyses& analyses = pass.analyses;
    Vector<GuardDependencies, 2> dependencies;
    auto relyOn = [&](const Value& test) {
        std::optional<GuardDependencies> dependenciesOfTest = dependenciesOfGuardOf(analyses, test);
        RELEASE_ASSERT(dependenciesOfTest);
        dependencies.append(WTF::move(*dependenciesOfTest));
    };
    relyOn(makeArrayStorageTest(array, ArrayStorage::Contiguous));
    if (!arm.namesAreProven)
        relyOn(makePristineTest(array, arm.builtinClass, { PristineAspects::Storage, PristineAspects::Names }));

    return anyOperationBetweenSteps(pass, iteration, liveRegion, [&](ValueID value) {
        GuardWriter writer(analyses, value);
        return std::ranges::any_of(dependencies, [&](const GuardDependencies& relied) { return writer.mayEnd(relied); });
    }, [&](BlockID block) {
        Effects effects = analyses.effectsOfTerminator(block).onAnyEdge();
        return effects.maySuspend || std::ranges::any_of(dependencies, [&](const GuardDependencies& relied) { return effects.writes.overlaps(relied.masks); });
    });
}

// The cleanup block's own handler, as a covered block has to name it: where the cleanup block passes on one of its parameters, the covered block passes what it passed for that parameter.
static std::optional<Target> findHandlerAround(const Function& function, const Target& toCleanupBlock)
{
    const Block& cleanupBlock = function.at(toCleanupBlock.block);
    RELEASE_ASSERT(toCleanupBlock.arguments.size() == cleanupBlock.parameters.size());
    std::optional<Target> outerHandler = cleanupBlock.handler;
    if (!outerHandler)
        return std::nullopt;
    for (ValueID& argument : outerHandler->arguments) {
        if (size_t parameterIndex = cleanupBlock.parameters.find(argument); parameterIndex != notFound)
            argument = toCleanupBlock.arguments[parameterIndex];
    }
    return outerHandler;
}

static bool operationsShareHandler(FunctionPassContext& pass, const Iteration& iteration)
{
    const Dominators& dominators = pass.analyses.dominators();
    const UseLists& useLists = pass.analyses.useLists();
    auto handlerAroundPattern = [&](BlockID block) {
        const std::optional<Target>& handler = pass.function.at(block).handler;
        return handler && iteration.cleanupBlocks.contains(handler->block) ? findHandlerAround(pass.function, *handler) : handler;
    };
    std::optional<Target> shared = handlerAroundPattern(useLists.pointOf(iteration.open).block);
    auto hasSharedHandler = [&](ValueID operation) {
        BlockID block = useLists.pointOf(operation).block;
        return isLandingCode(dominators, iteration, block) || handlerAroundPattern(block) == shared;
    };
    return std::ranges::all_of(iteration.resumes, hasSharedHandler) && std::ranges::all_of(iteration.closes, hasSharedHandler);
}

static PatternRead choosePatternRead(FunctionPassContext& pass, const Iteration& iteration, const IterationArm& arm, std::span<const BlockID> liveRegion)
{
    const Function& function = pass.function;
    const Value& open = function.at(iteration.open);
    std::optional<uint32_t> count = open.elementCount();
    if (iteration.kind == IteratorKind::Async || !open.isDestructuring() || open.hasRest() || !count || !*count)
        return PatternRead::BySteps;
    if (arm.form == IterationForm::Array) {
        // No read of an element runs code, and a read past the end or of a hole finds undefined, as a step past the end does. The test of the names has a failing edge, and the elements are tested with them.
        const ClassPart* part = arm.namesAreProven ? pass.program.typeOf(function, open.iterableOperand()).singleBuiltinClass() : nullptr;
        if (arm.kind != JSC::IterationKind::Values || (arm.namesAreProven && (!part || !part->provenPristine.contains(PristineAspects::Storage))))
            return PatternRead::BySteps;
        return stepsMatchElements(function, iteration, *count) && !regionMayChangeArray(pass, iteration, arm, liveRegion, open.iterableOperand()) ? PatternRead::ByIndex : PatternRead::BySteps;
    }
    if (arm.form != IterationForm::Protocol || pass.exceeds(LimitID::MaxDestructuredElements, *count, iteration.open) || !stepsMatchElements(function, iteration, *count))
        return PatternRead::BySteps;

    ValueID first = findFirstStep(function, iteration);
    bool firstStepDominatesSteps = std::ranges::all_of(iteration.resumes, [&](ValueID resume) { return resume == first || pass.analyses.dominates(first, pass.analyses.useLists().pointOf(resume)); });
    if (!firstStepDominatesSteps || !operationsShareHandler(pass, iteration))
        return PatternRead::BySteps;
    ProvenAccess method = provenReadOf(pass.program, pass.program.typeOf(function, open.iterableOperand()), pass.program.builtins().nameOf(WellKnownSymbol::Iterator));
    if (method.isComplete && !method.methods.isEmpty())
        return PatternRead::BySteps;

    bool regionInterferesWithSteps = anyOperationBetweenSteps(pass, iteration, liveRegion, [&](ValueID value) {
        return pass.analyses.effectsOf(value).onAnyEdge().interferesWith(Effects::unknownCode());
    }, [&](BlockID block) {
        return pass.analyses.effectsOfTerminator(block).onAnyEdge().interferesWith(Effects::unknownCode());
    });
    return regionInterferesWithSteps ? PatternRead::BySteps : PatternRead::InOneRead;
}

// Every point and every use is read before the first change, so the use lists are computed once for all patterns.
static void expandPatternsInOneRead(FunctionPassContext& pass, IRBuilder& builder, std::span<const PatternInOneRead> patterns)
{
    // What a pattern takes from the function as it stands before any pattern is changed.
    struct PlannedRead {
        ProgramPoint firstStep;
        ReplacedEffects replaced { };
        Vector<std::pair<ValueID, uint32_t>, 8> resultReadsWithStepIndex { };
        ValueID oneRead { };
    };
    if (patterns.empty())
        return;

    Function& function = pass.function;
    Vector<PlannedRead> plans;
    plans.reserveInitialCapacity(patterns.size());
    Vector<ProgramPoint> erasedPoints;
    for (const PatternInOneRead& pattern : patterns) {
        const UseLists& useLists = pass.analyses.useLists();
        PlannedRead plan { useLists.pointOf(findFirstStep(function, pattern.operations)) };
        plan.replaced.add(pass.analyses.effectContext(), function, function.at(pattern.operations.open));
        auto addOperation = [&](ValueID operation) {
            plan.replaced.add(pass.analyses.effectContext(), function, function.at(operation));
            erasedPoints.append(useLists.pointOf(operation));
        };
        for (ValueID resume : pattern.operations.resumes) {
            addOperation(resume);
            for (const Use& use : useLists.usesOf(resume)) {
                RELEASE_ASSERT(use.kind == Use::Kind::Operand && function.at(use.user).opcode == Opcode::ResultAt);
                plan.resultReadsWithStepIndex.append({ use.user, *function.at(resume).stepIndex() });
            }
        }
        for (ValueID close : pattern.operations.closes)
            addOperation(close);
        plans.append(WTF::move(plan));
    }

    AOT::sort(erasedPoints, { }, blockAndIndexOf);
    for (size_t first = 0; first < erasedPoints.size();) {
        BlockID block = erasedPoints[first].block;
        BitVector indexes;
        for (; first < erasedPoints.size() && erasedPoints[first].block == block; ++first)
            indexes.set(erasedPoints[first].index);
        builder.eraseValues(block, indexes);
    }

    auto numberOfPointsErasedBefore = [&](ProgramPoint point) { return static_cast<uint32_t>(std::ranges::lower_bound(erasedPoints, blockAndIndexOf(point), { }, blockAndIndexOf) - erasedPoints.begin()); };
    for (size_t index = 0; index < patterns.size(); ++index) {
        const Value& open = function.at(patterns[index].operations.open);
        ProgramPoint point = plans[index].firstStep;
        point.index -= numberOfPointsErasedBefore(point) - numberOfPointsErasedBefore(ProgramPoint::atHeadOf(point.block));
        builder.setOrigin(open.origin());
        plans[index].oneRead = builder.insertBefore(point, makeWith(Opcode::DestructureIterable, { patterns[index].iterable }, &Value::setCount, *open.elementCount()));
    }
    builder.applyInsertions();

    for (const PlannedRead& plan : plans) {
        checkReplacement(pass.analyses.effectContext(), function, ReplacementSite { pass.id, plan.firstStep.block, plan.oneRead }, function.at(plan.oneRead), plan.replaced, pass.build.diagnostics());
        for (auto [id, stepIndex] : plan.resultReadsWithStepIndex) {
            Value& read = function.edit(id);
            if (static_cast<IteratorResumeResult>(read.index()) == IteratorResumeResult::Done) {
                read.become(Opcode::Boolean);
                read.setBoolean(false);
            } else {
                RELEASE_ASSERT(static_cast<IteratorResumeResult>(read.index()) == IteratorResumeResult::Value);
                read.setResultsOperand(plan.oneRead);
                read.setIndex(stepIndex);
            }
        }
    }
    pass.analyses.didChangeValues();
}

// The blocks that close an iterator when an exception leaves the loop.

// Nothing but the cleanup block leads to the handler around it, so what stands there may read a parameter of the cleanup block by its name.
static void passParametersOfCleanupBlocksOn(FunctionPassContext& pass, IRBuilder& builder)
{
    Function& function = pass.function;
    bool didChange = false;
    for (uint32_t index = 0; index < function.blocks.size(); ++index) {
        BlockID cleanupBlock(index);
        if (function.at(cleanupBlock).role != BlockRole::IterationCleanup || !function.at(cleanupBlock).handler)
            continue;
        BlockID handlerAround = function.at(cleanupBlock).handler->block;
        if (!pass.analyses.cfg().isReachable(cleanupBlock) || !pass.analyses.cfg().singleIncomingEdge(handlerAround))
            continue;

        for (uint32_t parameterIndex = 0; parameterIndex < function.at(cleanupBlock).parameters.size(); ++parameterIndex) {
            ValueID parameter = function.at(cleanupBlock).parameters[parameterIndex];
            Vector<Use, 4> usesBehind;
            for (const Use& use : pass.analyses.useLists().usesOf(parameter)) {
                if (use.block != cleanupBlock)
                    usesBehind.append(use);
            }
            if (usesBehind.isEmpty())
                continue;

            ValueID passedOn = builder.addParameter(handlerAround, function.at(parameter).type);
            for (const Use& use : usesBehind)
                replaceUse(function, use, passedOn);
            builder.appendArgument(Edge::toHandlerOf(cleanupBlock), parameter);
            didChange = true;
        }
    }
    if (didChange)
        pass.analyses.didChangeValues();
}

// Every block names its handler, so one walk over the blocks finds the covered ones. Asking the CFG for the edges into a cleanup block would compute it again after every redirected edge.
static void dropCleanupRegions(FunctionPassContext& pass, IRBuilder& builder, std::span<const BlockID> cleanupBlocks)
{
    if (cleanupBlocks.empty())
        return;

    const Function& function = pass.function;
    BitVector isDropped(function.blocks.size());
    for (BlockID cleanupBlock : cleanupBlocks)
        isDropped.quickSet(cleanupBlock.index());
    auto leadsToDroppedBlock = [&](const std::optional<Target>& handler) { return handler && isDropped.quickGet(handler->block.index()); };

    for (uint32_t index = 0; index < function.blocks.size(); ++index) {
        BlockID coveredBlock(index);
        if (!leadsToDroppedBlock(function.at(coveredBlock).handler))
            continue;
        std::optional<Target> handler = function.at(coveredBlock).handler;
        while (leadsToDroppedBlock(handler))
            handler = findHandlerAround(function, *handler);
        if (handler)
            builder.redirectEdge(Edge::toHandlerOf(coveredBlock), WTF::move(*handler));
        else
            builder.removeHandler(coveredBlock);
    }
    builder.emptyUnreachableBlocks();
}

// The iterations of a function.

static bool provesNotDone(const Function& function, Edge edge, ValueID resume)
{
    constexpr uint32_t notTakenTargetIndex = 1;
    if (edge.isHandlerEdge() || edge.targetIndex != notTakenTargetIndex || !function.at(edge.from).terminator.isConditionalBranch())
        return false;
    const Value& condition = function.at(function.at(edge.from).terminator.conditionOperand());
    return condition.opcode == Opcode::ResultAt && condition.resultsOperand() == resume && condition.index() == doneResult;
}

static bool awaitsCallOnString(FunctionPassContext& pass, const Iteration& iteration)
{
    Vector<ValueID, 2> chain = findSingleUseCastChain(pass.function, pass.analyses.useLists(), iteration.open);
    if (chain.isEmpty())
        return false;
    const Value& call = pass.function.at(chain.first());
    return call.opcode == Opcode::Call && call.calleeKey() && call.thisValueOperand() && pass.program.typeOf(pass.function, call.thisValueOperand()).isAnyString();
}

static bool isLastUseOfOpen(FunctionAnalyses& analyses, ValueID open, const BitVector& liveRegion, ValueLocation close)
{
    const UseLists& useLists = analyses.useLists();
    bool isLiveAtTail = std::ranges::any_of(analyses.cfg().successors(close.block), [&](BlockID successor) { return liveRegion.quickGet(successor.index()); });
    return !isLiveAtTail && std::ranges::none_of(useLists.usesOf(open), [&](const Use& use) { return use.kind == Use::Kind::Operand && use.block == close.block && useLists.locationOf(use.user).index > close.index; });
}

// Whether every arm gets its own copy of the blocks in which the iterator is live. Where it does not, every operation asks which arm was taken.
static bool canCopyRegionPerArm(FunctionPassContext& pass, const Iteration& iteration, const IterationPlan& plan, std::span<const BlockID> liveRegion)
{
    if (plan.arms.size() == 1 || liveRegion.empty())
        return true;
    if (RegionCloner::checkCanDuplicate(pass.analyses, liveRegion) != CloneCheck::CanClone)
        return false;

    uint64_t bytes = 0;
    for (BlockID block : liveRegion)
        bytes += estimatedCodeBytes(pass.program, pass.function, pass.function.at(block));
    return !pass.exceeds(LimitID::MaxVersionedIterationBytes, bytes * (plan.arms.size() - 1), iteration.open);
}

namespace {

struct ArmCopy {
    BlockID entry;
    Iteration operations;
    bool isReadAtOnce { false };
};

struct OperationSite {
    ProgramPoint point;
    ValueID value;
    unsigned copy;
    Vector<ValueID, 2> resultReads;
};

// One iteration of a round. Every step is taken for all iterations of the round before the next step is: the steps that only read the function share what the analyses have computed.
struct ArmDispatcher {
    FunctionPassContext& pass;
    IRBuilder& builder;
    SSARepair& repair;
    Iteration iteration;
    Function& function { pass.function };
    FunctionAnalyses& analyses { pass.analyses };
    IterationExpansion expansion { &pass, pass.program, pass.function, pass.types, builder, &repair, iteration.origin };
    Vector<BlockID> region { findLiveRegion(pass, iteration.open) };
    ValueLocation open { analyses.useLists().locationOf(iteration.open) };
    BlockID head { open.block };
    BlockID tail { function.at(head).terminator.next().block };
    IterationPlan plan { };
    bool mayBeUsedAfterDone { true };
    Vector<ProgramPoint, 2> cutsAfterLastCloses { };
    bool hasCopyPerArm { true };
    Vector<PatternRead, 2> patternReads { };
    Vector<ArmCopy, 2> copies { };
    Vector<ValueID> definitionsInRegion { };
    Vector<Vector<ValueID>, 1> copiesOfDefinitions { };
    Vector<ArmState, 2> states { };
    Vector<BlockID, 2> ends { };
    ValueID form { };
    std::optional<StateVariable> done { };
    Vector<OperationSite, 8> sites { };

    void planArms()
    {
        RELEASE_ASSERT(open.placement == ValuePlacement::InBlock && function.at(head).terminator.next().arguments.isEmpty());
        plan = planIteration(pass, iteration);
        BitVector liveRegion = blockSetOf(function, region.span());
        mayBeUsedAfterDone = findsOperationAfterDone(liveRegion);
        if (plan.arms.size() <= 1)
            return;

        // A block that was merged with its successor goes on with the code after the iteration, which no copy needs and no limit on copies should count.
        for (ValueID close : iteration.closes) {
            ValueLocation location = analyses.useLists().locationOf(close);
            if (location.index + 1 < function.at(location.block).values.size() && isLastUseOfOpen(analyses, iteration.open, liveRegion, location))
                cutsAfterLastCloses.append(ProgramPoint { location.block, location.index + 1 });
        }
    }

    bool findsOperationAfterDone(const BitVector& liveRegion)
    {
        const UseLists& useLists = analyses.useLists();
        Vector<ValueLocation, 8> operations;
        for (ValueID resume : iteration.resumes)
            operations.append(useLists.locationOf(resume));
        for (ValueID close : iteration.closes)
            operations.append(useLists.locationOf(close));

        for (size_t index = 0; index < iteration.resumes.size(); ++index) {
            ValueLocation step = operations[index];
            // CFG::blocksReachableFrom stops at blocks. What ends this walk is an edge: the one taken while the step is not done, and any edge to a block in which the iterator is dead, from where only the open leads back.
            BitVector enteredBlocks(function.blocks.size());
            Vector<BlockID, 16> worklist { step.block };
            while (!worklist.isEmpty()) {
                forEachOutgoingEdge(function, worklist.takeLast(), [&](Edge edge, const Target& target) {
                    if (liveRegion.quickGet(target.block.index()) && !provesNotDone(function, edge, iteration.resumes[index]) && !enteredBlocks.set(target.block.index()))
                        worklist.append(target.block);
                });
            }
            if (operations.containsIf([&](const ValueLocation& operation) { return enteredBlocks.get(operation.block.index()) || (operation.block == step.block && operation.index > step.index); }))
                return true;
        }
        return false;
    }

    void splitAfterLastCloses()
    {
        builder.setOrigin(function.at(iteration.open).origin());
        for (ProgramPoint point : cutsAfterLastCloses)
            builder.splitBlock(point);
    }

    void chooseForms()
    {
        hasCopyPerArm = canCopyRegionPerArm(pass, iteration, plan, region.span());
        bool readsPattern = function.at(iteration.open).isDestructuring() && hasCopyPerArm;
        for (const IterationArm& arm : plan.arms)
            patternReads.append(readsPattern ? choosePatternRead(pass, iteration, arm, region.span()) : PatternRead::BySteps);
    }

    void emitOpens()
    {
        builder.setOrigin(function.at(iteration.open).origin());
        cloneRegionPerArm();

        function.edit(head, { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments }).terminator = Terminator();
        analyses.didChangeControlFlow();
        builder.setHandler(function.at(head).handler);
        builder.moveToEndOf(head);
        if (mayBeUsedAfterDone && patternReads.contains(PatternRead::BySteps))
            done = expansion.addVariable(builder.appendBoolean(false));

        if (plan.armSources.isEmpty())
            emitTestsAndOpens();
        else
            emitOpensByArmSource();
        if (!hasCopyPerArm) {
            joinArmsByForm();
            return;
        }
        RELEASE_ASSERT(ends.size() == copies.size());
        for (size_t arm = 0; arm < ends.size(); ++arm) {
            builder.moveToEndOf(ends[arm]);
            builder.emitJump(copies[arm].entry);
        }
    }

    void cloneRegionPerArm()
    {
        copies.append(ArmCopy { tail, iteration });
        if (!hasCopyPerArm || plan.arms.size() == 1)
            return;

        for (BlockID block : region) {
            definitionsInRegion.append(function.at(block).parameters.span());
            definitionsInRegion.append(function.at(block).values.span());
        }

        for (size_t arm = 1; arm < plan.arms.size(); ++arm) {
            if (region.isEmpty()) {
                copies.append(ArmCopy { tail, { .open = iteration.open, .kind = iteration.kind, .origin = iteration.origin } });
                continue;
            }

            RegionCloner cloner(builder, function);
            cloner.cloneBlocks(region.span(), [](ValueID, Value&) { });
            auto cloneOf = [&](auto original) { return cloner.cloneOf(original); };
            copies.append(ArmCopy { cloneOf(tail), { iteration.open, iteration.kind, iteration.origin, Vector<ValueID, 2>::map(iteration.resumes, cloneOf), Vector<ValueID, 2>::map(iteration.closes, cloneOf), Vector<BlockID, 1>::map(iteration.cleanupBlocks, cloneOf) } });
            RELEASE_ASSERT(copies.last().entry);

            if (plan.arms[arm].form == IterationForm::Protocol) {
                for (BlockID block : cloner.clonedBlocks())
                    function.edit(block, BlockEdit::Rest).frequency = rarerOf(function.at(block).frequency, BlockFrequency::Unlikely);
                analyses.didChangeControlFlow();
            }

            // A cloner has a word for every value and every block of the function, so it does not wait for the repair.
            copiesOfDefinitions.append(definitionsInRegion.map(cloneOf));
        }
    }

    // One variable holds a definition and all of its copies: an edge out of one copy carries no definition of a variable that knows only another copy.
    void addVariablesOfCopies()
    {
        if (copiesOfDefinitions.isEmpty())
            return;

        const UseLists& useLists = analyses.useLists();
        auto isUsedOutsideBlock = [&](ValueID definition) { return std::ranges::any_of(useLists.usesOf(definition), [&, block = useLists.blockOf(definition)](const Use& use) { return use.block != block; }); };
        for (size_t index = 0; index < definitionsInRegion.size(); ++index) {
            if (!isUsedOutsideBlock(definitionsInRegion[index]) && std::ranges::none_of(copiesOfDefinitions, [&](const Vector<ValueID>& copies) { return isUsedOutsideBlock(copies[index]); }))
                continue;
            SSARepair::Variable variable = repair.addVariable(definitionsInRegion[index]);
            for (const Vector<ValueID>& copies : copiesOfDefinitions)
                repair.addDefinition(variable, copies[index]);
        }
    }

    void openArm(const IterationArm& arm, ValueID subject)
    {
        states.append(ArmState { .arm = arm, .patternRead = patternReads[states.size()], .subject = subject });
        ArmState& state = states.last();
        switch (arm.form) {
        case IterationForm::Array:
        case IterationForm::TypedArray:
        case IterationForm::String:
        case IterationForm::SplitString:
        case IterationForm::OwnEntries:
            emitIndexedOpen(expansion, state);
            break;
        case IterationForm::Map:
        case IterationForm::Set:
            state.table = expansion.addVariable(builder.append(Opcode::CollectionTable, { subject }));
            state.position = expansion.addVariable(builder.appendInteger(0));
            break;
        case IterationForm::Generator:
        case IterationForm::AsyncGenerator:
            break;
        case IterationForm::Protocol:
            if (state.patternRead == PatternRead::InOneRead)
                copies.last().isReadAtOnce = true;
            else
                emitProtocolOpen(expansion, state, iteration.kind);
            break;
        }
        ends.append(builder.currentBlock());
    }

    void emitTestsAndOpens()
    {
        BlockID protocolEntry;
        ValueID protocolIterable = plan.source;
        if (plan.hasProtocolArm() && plan.hasClassArm()) {
            protocolEntry = builder.addBlock(rarerOf(function.at(head).frequency, BlockFrequency::Unlikely));
            protocolIterable = addParameterLike(pass, builder, protocolEntry, plan.source);
        }

        ValueID rest = plan.source;
        for (const IterationArm& arm : plan.classArms()) {
            RELEASE_ASSERT(rest);
            rest = emitClassArm(arm, rest, protocolEntry);
        }
        if (plan.hasClassArm() && rest && protocolEntry)
            builder.emitJump(protocolEntry, { rest });
        else if (plan.hasClassArm() && rest) {
            builder.append(makeRequireObjectCoercible(rest, iteration.origin));
            builder.emitUnreachable();
        }

        if (!plan.hasProtocolArm())
            return;
        if (protocolEntry)
            builder.moveToEndOf(protocolEntry);
        openArm(plan.arms.last(), protocolIterable);
    }

    // Leaves the builder where the test of the class has failed and answers what the value is there.
    ValueID emitClassArm(const IterationArm& arm, ValueID value, BlockID protocolEntry)
    {
        IRBuilder::ProvenBranch classTest { .provenOnTaken = value };
        if (!arm.classIsProven)
            classTest = builder.emitBranchWithProof(builder.appendTypeTest(value, arm.testedType), 0, BlockFrequency::Normal, BlockFrequency::Unlikely);
        ValueID subject = classTest.provenOnTaken;

        if (!arm.namesAreProven) {
            RELEASE_ASSERT(protocolEntry);
            OptionSet<PristineAspects> aspects { PristineAspects::Names };
            if (patternReads[states.size()] == PatternRead::ByIndex)
                aspects.add(PristineAspects::Storage);
            builder.emitFailureOfPredictions(Target { protocolEntry, { chooseArgumentOfProtocolEntry(pass, protocolEntry, subject, value) }, { } });
            IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(builder.append(makeNamesTest(arm, subject, aspects)), 0, BlockFrequency::Normal, BlockFrequency::Unlikely);
            builder.moveToEndOf(branch.notTaken);
            builder.emitJump(protocolEntry, { chooseArgumentOfProtocolEntry(pass, protocolEntry, subject, value) });
            builder.moveToEndOf(branch.taken);
        }

        openArm(arm, subject);
        if (classTest.notTaken)
            builder.moveToEndOf(classTest.notTaken);
        return classTest.provenOnNotTaken;
    }

    // One block for each arm, entered by the number of the arm.
    Vector<BlockID, 4> emitSwitchOverArms(ValueID chosenArm, BlockFrequency frequency)
    {
        Vector<BlockID, 4> blocks;
        for (const IterationArm& arm : plan.arms)
            blocks.append(builder.addBlock(arm.form == IterationForm::Protocol ? rarerOf(frequency, BlockFrequency::Unlikely) : frequency));

        Vector<std::pair<int64_t, Target>> cases;
        for (size_t index = 0; index + 1 < blocks.size(); ++index)
            cases.constructAndAppend(static_cast<int64_t>(index), Target { blocks[index], { }, { } });
        builder.emitSwitch(chosenArm, WTF::move(cases), Target { blocks.last(), { }, { } });
        return blocks;
    }

    // Every definition of a subject has the type of its arm: an operation is typed from the definition it names, before SSARepair picks the one that reaches it.
    void emitOpensByArmSource()
    {
        std::optional<StateVariable> chosenArm;
        Vector<std::optional<StateVariable>, 2> subjects(plan.classArms().size());
        auto define = [&](std::optional<StateVariable>& variable, ProgramPoint point, Value&& definition) {
            ValueID id = builder.insertBefore(point, WTF::move(definition));
            if (variable)
                expansion.define(*variable, id);
            else
                variable = expansion.addVariable(id);
        };
        for (const ArmSource& source : plan.armSources) {
            ProgramPoint point = pointBeforeTerminatorOf(function, source.block);
            if (plan.arms.size() > 1)
                define(chosenArm, point, makeInteger(source.arm));

            // SSARepair rewrites every use of a definition, and the program's own uses of the receiver have to keep naming it.
            for (size_t index = 0; index < subjects.size(); ++index) {
                TypeID type = plan.arms[index].testedType;
                define(subjects[index], point, index == source.arm ? makeWith(Opcode::Upcast, { source.subject }, &Value::setCastType, type) : makeWith(Opcode::Unspecified, { }, &Value::setValueType, type));
            }

            if (source.passedIterator) {
                TypeID type = function.at(usedValueOf(function, *source.passedIterator)).type;
                replaceUse(function, *source.passedIterator, builder.insertBefore(point, makeWith(Opcode::Unspecified, { }, &Value::setValueType, type)));
            }
        }
        builder.applyInsertions();

        if (plan.arms.size() == 1) {
            openArm(plan.arms.first(), subjects.first()->anyDefinition);
            return;
        }
        Vector<BlockID, 4> entries = emitSwitchOverArms(chosenArm->anyDefinition, function.at(head).frequency);
        for (size_t arm = 0; arm < entries.size(); ++arm) {
            builder.moveToEndOf(entries[arm]);
            openArm(plan.arms[arm], arm < subjects.size() ? subjects[arm]->anyDefinition : plan.source);
        }
    }

    void joinArmsByForm()
    {
        static constexpr ValueID ArmState::* joinedValues[] { &ArmState::subject, &ArmState::nextMethod, &ArmState::isSteppedWithoutCall };
        BlockID join = builder.addBlock(function.at(head).frequency);
        form = builder.addParameter(join, pass.types.intern(Type::integer({ 0, static_cast<int64_t>(states.size()) - 1 })));

        Vector<ArmState, 2> joinedStates = states;
        for (ArmState& state : joinedStates) {
            for (auto value : joinedValues) {
                if (state.*value)
                    state.*value = addParameterLike(pass, builder, join, state.*value);
            }
        }

        for (size_t arm = 0; arm < states.size(); ++arm) {
            builder.moveToEndOf(ends[arm]);
            Arguments arguments { builder.appendInteger(static_cast<int64_t>(arm)) };
            for (size_t other = 0; other < states.size(); ++other) {
                const ArmState& state = states[other];
                for (auto value : joinedValues) {
                    if (state.*value)
                        arguments.append(other == arm ? state.*value : expansion.appendUnspecifiedLike(state.*value));
                }
                for (const std::optional<StateVariable>& variable : { state.position, state.table }) {
                    if (variable && other != arm)
                        expansion.define(*variable, expansion.appendUnspecifiedLike(variable->anyDefinition));
                }
            }
            builder.emitJump(join, WTF::move(arguments));
        }

        states = WTF::move(joinedStates);
        builder.moveToEndOf(join);
        builder.emitJump(tail);
    }

    void collectPatternsReadAtOnce(Vector<PatternInOneRead>& patterns)
    {
        for (size_t index = 0; index < copies.size(); ++index) {
            if (copies[index].isReadAtOnce)
                patterns.append(PatternInOneRead { copies[index].operations, states[index].subject });
        }
    }

    void collectOperationSites()
    {
        const UseLists& useLists = analyses.useLists();
        // An operation that nothing reaches any more goes away with its block.
        auto addSite = [&](ValueID operation, unsigned copy) {
            ValueLocation location = useLists.locationOf(operation);
            if (location.placement != ValuePlacement::InBlock)
                return;
            OperationSite site { { location.block, location.index }, operation, copy, { } };
            for (const Use& use : useLists.usesOf(operation)) {
                if (use.kind == Use::Kind::Operand && function.at(use.user).opcode == Opcode::ResultAt)
                    site.resultReads.append(use.user);
            }
            sites.append(WTF::move(site));
        };
        for (unsigned copy = 0; copy < copies.size(); ++copy) {
            if (copies[copy].isReadAtOnce)
                continue;
            for (ValueID resume : copies[copy].operations.resumes)
                addSite(resume, copy);
            for (ValueID close : copies[copy].operations.closes)
                addSite(close, copy);
        }
        AOT::sort(sites, std::ranges::greater { }, [](const OperationSite& site) { return blockAndIndexOf(site.point); });
    }

    void expandOperations()
    {
        for (const OperationSite& site : sites) {
            RELEASE_ASSERT(function.at(site.point.block).values[site.point.index] == site.value);
            Value original = function.at(site.value);
            bool isStep = original.opcode == Opcode::IteratorResume;
            builder.setOrigin(original.origin());
            // The overload without reads looks them up in the whole function and erases them wherever they stand, which moves the points of the sites still to come.
            IRBuilder::Expansion operation = isStep ? builder.beginExpansion(site.point, site.resultReads.span()) : builder.beginExpansion(site.point);
            expansion.operation = &operation;

            bool tracksDone = done && isStep && (!hasCopyPerArm || states[site.copy].patternRead == PatternRead::BySteps);
            if (tracksDone) {
                IRBuilder::BranchBlocks blocks = builder.emitBranch(done->anyDefinition);
                expansion.finishStepAsDone();

                // A step that throws leaves the state done: this is the definition its handler sees.
                builder.moveToEndOf(blocks.notTaken);
                expansion.define(*done, builder.appendBoolean(true));
            } else if (done && !isStep && !closingHasNoEffectIn(site.copy)) {
                IRBuilder::BranchBlocks blocks = builder.emitBranch(done->anyDefinition);
                expansion.finishClose();
                builder.moveToEndOf(blocks.notTaken);
            }

            if (hasCopyPerArm)
                emitOperation(original, states[site.copy]);
            else {
                Vector<BlockID, 4> blocks = emitSwitchOverArms(form, function.at(builder.currentBlock()).frequency);
                for (size_t arm = 0; arm < blocks.size(); ++arm) {
                    builder.moveToEndOf(blocks[arm]);
                    emitOperation(original, states[arm]);
                }
            }

            if (tracksDone) {
                // SSARepair rewrites every use of a definition, and the program's own uses of this step's result have to keep naming it.
                expansion.define(*done, builder.insertBefore(ProgramPoint::atHeadOf(operation.continuation), makeWith(Opcode::Upcast, { operation.results[doneResult] }, &Value::setCastType, TypeTable::boolean)));
                builder.applyInsertions();
            }
        }
    }

    void emitOperation(const Value& original, ArmState& state)
    {
        if (original.opcode == Opcode::IteratorClose) {
            // %AsyncFromSyncIteratorPrototype%.return finds no method to call on the iterator of such an arm and answers with a fulfilled promise, which the close awaits.
            if (state.arm.awaitsEachValue)
                appendAwait(builder, builder.appendUndefined());
            if (state.arm.awaitsEachValue || !state.arm.hasMethodsToCall())
                expansion.finishClose();
            else if (state.arm.form == IterationForm::Generator)
                emitGeneratorClose(expansion, state, original.completion());
            else
                emitProtocolClose(expansion, state, original.iteratorKind(), original.completion());
            return;
        }

        RELEASE_ASSERT(original.opcode == Opcode::IteratorResume);
        StepRequest request { original.iteratorKind(), original.resumeMode(), original.sentOperand(), original.readsValueWhenDone() };
        if (state.arm.awaitsEachValue && request.mode == ResumeMode::Next)
            emitStepOfWrappedIterator(state);
        else if (state.arm.awaitsEachValue) {
            ArmState protocol { .arm = IterationArm { }, .subject = openWrappedIterator(expansion, state.subject) };
            request.kind = IteratorKind::Async;
            emitProtocolStep(expansion, protocol, request);
        } else if (state.patternRead == PatternRead::ByIndex) {
            ValueID index = builder.appendInteger(*original.stepIndex());
            ValueID isPastEnd = builder.append(Opcode::LogicalNot, { builder.append(Opcode::Less, { index, expansion.appendLength(state.subject) }) });
            expansion.finishStep(isPastEnd, expansion.appendElement(state.subject, index));
        } else
            emitArmStep(expansion, state, request);
    }

    // %AsyncFromSyncIteratorPrototype%.next rejects its promise with what the step throws, and the loop awaits that promise: the exception arrives one turn of the job queue later.
    void emitStepOfWrappedIterator(ArmState& state)
    {
        BlockFrequency frequency = function.at(builder.currentBlock()).frequency;
        std::optional<Target> outer = builder.handler();

        IRBuilder::Expansion step { builder.addBlock(frequency), { }, outer };
        step.results.append(builder.addParameter(step.continuation, TypeTable::boolean));
        step.results.append(builder.addParameter(step.continuation, TypeTable::anything));

        BlockID thrown = builder.addHandlerBlock(BlockRole::CleanupHandler, ValueID());
        builder.setHandler(Target { thrown, { }, { } });
        BlockID attempt = builder.addBlock(frequency);
        builder.emitJump(attempt);

        builder.moveToEndOf(attempt);
        const IRBuilder::Expansion* operation = std::exchange(expansion.operation, &step);
        emitArmStep(expansion, state, StepRequest { });
        expansion.operation = operation;
        builder.setHandler(WTF::move(outer));

        builder.moveToEndOf(thrown);
        ValueID exception = builder.append(Opcode::CatchException, { });
        appendAwait(builder, builder.appendUndefined());
        builder.emitRethrow(exception);

        builder.moveToEndOf(step.continuation);
        bool isInAsyncGenerator = function.role == FunctionRole::CoroutineBody ? coroutineOfBody(function).kind == CoroutineKind::AsyncGenerator : function.has(FunctionFlags::IsAsync) && function.has(FunctionFlags::IsGenerator);
        if (!isInAsyncGenerator) {
            ValueID value = appendAwait(builder, step.results[valueResult]);
            function.edit(value, ValueEdit::Definition).setTakesExtraTurn(true);
            expansion.finishStep(step.results[doneResult], value);
            return;
        }

        outer = builder.handler();
        builder.setHandler(Target { thrown, { }, { } });
        BlockID awaiting = builder.addBlock(frequency);
        builder.setHandler(WTF::move(outer));
        BlockID awaited = builder.addBlock(frequency);
        ValueID value = builder.addParameter(awaited, TypeTable::anything);
        builder.emitJump(awaiting);

        builder.moveToEndOf(awaiting);
        builder.emitJump(awaited, { appendAwait(builder, step.results[valueResult]) });

        builder.moveToEndOf(awaited);
        appendAwait(builder, builder.appendUndefined());
        expansion.finishStep(step.results[doneResult], value);
    }

    void finishCleanupBlocks(Vector<BlockID>& cleanupBlocksToDrop)
    {
        for (unsigned copy = 0; copy < copies.size(); ++copy) {
            if (closingHasNoEffectIn(copy)) {
                cleanupBlocksToDrop.append(copies[copy].operations.cleanupBlocks.span());
                continue;
            }
            bool didRelease = false;
            for (BlockID cleanupBlock : copies[copy].operations.cleanupBlocks) {
                Block& block = function.edit(cleanupBlock);
                if (block.role != BlockRole::IterationCleanup)
                    continue;
                block.role = BlockRole::CleanupHandler;
                block.subject = ValueID();
                didRelease = true;
            }
            if (didRelease)
                analyses.didChangeValues();
        }
    }

    bool closingHasNoEffectIn(unsigned copy) const
    {
        if (copies[copy].isReadAtOnce)
            return true;
        if (hasCopyPerArm)
            return plan.arms[copy].closingHasNoEffect();
        return std::ranges::all_of(plan.arms, &IterationArm::closingHasNoEffect);
    }

    void eraseOpenAndReport()
    {
        const BlockValues& values = function.at(head).values;
        RELEASE_ASSERT(values[open.index] == iteration.open);
        BitVector indexes;
        indexes.set(open.index);
        for (uint32_t index = 0; index < open.index; ++index) {
            if (plan.replacedValues.contains(values[index]))
                indexes.set(index);
        }
        RELEASE_ASSERT(indexes.bitCount() == plan.replacedValues.size() + 1);
        builder.eraseValues(head, indexes);

        pass.build.statistics().count(plan.hasClassArm() ? StatisticID::IterationsExpandedByClass : StatisticID::IterationsExpandedGenerically);
        if (iteration.kind == IteratorKind::Sync && !plan.hasClassArm())
            pass.addDecline(Wire::DeclineKind::IterationNotExpandedByClass, iteration.open);
    }
};

using ArmDispatchers = Vector<std::unique_ptr<ArmDispatcher>>;

} // anonymous namespace

// Every open ends its block. A close that is followed by an operation of another iteration ends its block as well, so iterations that stand one behind the other in straight-line code have no block in common.
static bool separateIterations(FunctionPassContext& pass, IRBuilder& builder, std::span<const Iteration> iterations, BitVector& separatedOpens)
{
    const UseLists& useLists = pass.analyses.useLists();
    Vector<std::pair<ProgramPoint, Origin>, 16> cuts;
    for (const Iteration& iteration : iterations) {
        auto cutBehind = [&](ValueLocation location) {
            cuts.append({ { location.block, location.index + 1 }, pass.function.at(iteration.open).origin() });
        };
        if (!separatedOpens.set(iteration.open.index()))
            cutBehind(useLists.locationOf(iteration.open));

        std::optional<BitVector> liveRegion;
        for (ValueID close : iteration.closes) {
            ValueLocation location = useLists.locationOf(close);
            if (std::ranges::none_of(pass.function.at(location.block).values.span().subspan(location.index + 1), [&](ValueID id) { return removedByOf(pass.function.at(id).opcode) == PassID::ExpandIteration; }))
                continue;
            if (!liveRegion)
                liveRegion = blockSetOf(pass.function, findLiveRegion(pass, iteration.open).span());
            if (isLastUseOfOpen(pass.analyses, iteration.open, *liveRegion, location))
                cutBehind(location);
        }
    }

    // From the last cut of a block to the first: what stands in front of a cut stays where it is.
    AOT::sort(cuts, std::ranges::greater { }, [](const auto& cut) { return blockAndIndexOf(cut.first); });
    for (auto& [point, origin] : cuts) {
        builder.setOrigin(origin);
        builder.splitBlock(point);
    }
    return !cuts.isEmpty();
}

// What an arm defines at the end of a block that passes an iterator on has to find that block as the plan saw it.
static void selectByPlans(FunctionPassContext& pass, ArmDispatchers& dispatchers, ExpandedIterations expandedIterations)
{
    constexpr unsigned nobody = 0;
    Vector<unsigned> ownerOfBlock(FillWith { }, pass.function.blocks.size(), nobody);
    for (unsigned index = 0; index < dispatchers.size(); ++index) {
        ownerOfBlock[dispatchers[index]->head.index()] = index + 1;
        for (BlockID block : dispatchers[index]->region)
            ownerOfBlock[block.index()] = index + 1;
    }

    BitVector isPostponed(dispatchers.size());
    bool isAnyLeftOut = false;
    for (unsigned index = 0; index < dispatchers.size(); ++index) {
        const IterationPlan& plan = dispatchers[index]->plan;
        if (expandedIterations == ExpandedIterations::OnlyProven && !plan.isExpandedWithoutTest()) {
            dispatchers[index] = nullptr;
            isAnyLeftOut = true;
            continue;
        }

        if (std::ranges::any_of(plan.armSources, [&](const ArmSource& source) { return ownerOfBlock[source.block.index()] != nobody && ownerOfBlock[source.block.index()] != index + 1; })) {
            isPostponed.quickSet(index);
            continue;
        }
        for (const ArmSource& source : plan.armSources)
            ownerOfBlock[source.block.index()] = index + 1;
    }

    // Alone, the last iteration shares no block with anybody.
    if (!isAnyLeftOut && !dispatchers.isEmpty() && isPostponed.bitCount() == dispatchers.size())
        isPostponed.quickClear(0);
    for (size_t index : isPostponed)
        dispatchers[index] = nullptr;
    dispatchers.removeAllMatching([](const std::unique_ptr<ArmDispatcher>& dispatcher) { return !dispatcher; });
}

// The iterations of a function that have no block in common are expanded together: the analyses are computed a fixed number of times for all of them, and SSA is repaired once.
static bool expandRoundOfIterations(FunctionPassContext& pass, IRBuilder& builder, std::span<const ValueID> opensOfSpreads, ExpandedIterations expandedIterations, BitVector& separatedOpens, PassOutcome& outcome)
{
    SSARepair repair(builder, pass.analyses);
    Vector<Iteration> iterations = collectIterations(pass);
    for (Iteration& iteration : iterations) {
        if (std::ranges::contains(opensOfSpreads, iteration.open))
            iteration.origin = IterationOrigin::Spread;
    }
    if (expandedIterations == ExpandedIterations::OnlyProven)
        iterations.removeAllMatching([&](const Iteration& iteration) { return awaitsCallOnString(pass, iteration) || !planIteration(pass, iteration).isExpandedWithoutTest(); });
    if (iterations.isEmpty())
        return false;
    if (separateIterations(pass, builder, iterations.span(), separatedOpens))
        outcome = PassOutcome::Changed;

    // From the last iteration to the first, so that an iteration is asked after the ones nested in it.
    ArmDispatchers dispatchers;
    BitVector takenBlocks(pass.function.blocks.size());
    for (const Iteration& iteration : iterations | std::views::reverse) {
        auto dispatcher = makeUniqueWithoutFastMallocCheck<ArmDispatcher>(pass, builder, repair, iteration);
        bool sharesBlock = takenBlocks.quickSet(dispatcher->head.index());
        for (BlockID block : dispatcher->region)
            sharesBlock |= takenBlocks.quickSet(block.index());
        if (!sharesBlock)
            dispatchers.append(WTF::move(dispatcher));
    }
    auto forEachDispatcher = [&](auto step, auto&... arguments) {
        for (const std::unique_ptr<ArmDispatcher>& dispatcher : dispatchers)
            (dispatcher.get()->*step)(arguments...);
    };

    forEachDispatcher(&ArmDispatcher::planArms);
    selectByPlans(pass, dispatchers, expandedIterations);
    if (dispatchers.isEmpty())
        return false;
    outcome = PassOutcome::Changed;

    passParametersOfCleanupBlocksOn(pass, builder);
    forEachDispatcher(&ArmDispatcher::splitAfterLastCloses);
    forEachDispatcher(&ArmDispatcher::chooseForms);
    forEachDispatcher(&ArmDispatcher::emitOpens);
    forEachDispatcher(&ArmDispatcher::addVariablesOfCopies);
    Vector<PatternInOneRead> patternsReadAtOnce;
    forEachDispatcher(&ArmDispatcher::collectPatternsReadAtOnce, patternsReadAtOnce);
    expandPatternsInOneRead(pass, builder, patternsReadAtOnce.span());
    forEachDispatcher(&ArmDispatcher::collectOperationSites);
    forEachDispatcher(&ArmDispatcher::expandOperations);

    Vector<BlockID> cleanupBlocksToDrop;
    forEachDispatcher(&ArmDispatcher::finishCleanupBlocks, cleanupBlocksToDrop);
    dropCleanupRegions(pass, builder, cleanupBlocksToDrop.span());

    forEachDispatcher(&ArmDispatcher::eraseOpenAndReport);
    builder.setHandler(std::nullopt);
    repair.run();
    builder.emptyUnreachableBlocks();
    return true;
}

static PassOutcome expandIterations(FunctionPassContext& pass, IRBuilder& builder, std::span<const ValueID> opensOfSpreads, ExpandedIterations expandedIterations)
{
    BitVector separatedOpens;
    PassOutcome outcome = PassOutcome::Unchanged;
    while (expandRoundOfIterations(pass, builder, opensOfSpreads, expandedIterations, separatedOpens, outcome)) { }
    return outcome;
}

// Spreads.

static void expandSpread(FunctionPassContext& pass, IRBuilder& builder, ProgramPoint point, const IterationPlan& plan, Vector<SpreadLoop>& loops)
{
    Function& function = pass.function;
    ValueID id = function.at(point.block).values[point.index];
    ReplacedEffects replaced;
    auto checkAppendElements = [&](BlockID block, ValueID appendElements) {
        checkReplacement(pass.analyses.effectContext(), function, { pass.id, block, appendElements }, function.at(appendElements), replaced, pass.build.diagnostics());
    };

    RELEASE_ASSERT(plan.hasClassArm() || plan.hasProtocolArm());
    if (plan.hasClassArm() && plan.arms.first().needsNoTest()) {
        Value& value = function.edit(id);
        replaced.add(pass.analyses.effectContext(), function, value);
        ValueID array = value.arrayOperand();
        ValueID source = value.iterableOperand();
        value.become(Opcode::AppendElements);
        value.setArrayOperand(array);
        value.setSourceOperand(source);
        pass.analyses.didChangeValues();
        checkAppendElements(point.block, id);
        return;
    }

    Value original = function.at(id);
    ValueID iterable = original.iterableOperand();
    replaced.add(pass.analyses.effectContext(), function, original);
    BlockFrequency loopFrequency = rarerOf(plan.hasClassArm() ? BlockFrequency::Unlikely : BlockFrequency::Normal, function.at(point.block).frequency);

    builder.setOrigin(original.origin());
    IRBuilder::Expansion cut = builder.beginExpansion(point);
    BlockID loopEntry = plan.hasProtocolArm() ? builder.addBlock(loopFrequency) : BlockID();
    ValueID source = loopEntry ? addParameterLike(pass, builder, loopEntry, iterable) : ValueID();

    if (plan.hasClassArm()) {
        const IterationArm& arm = plan.arms.first();
        RELEASE_ASSERT(arm.form == IterationForm::Array);
        Value appendElements = IRBuilder::make(Opcode::AppendElements, { original.arrayOperand(), iterable });
        if (!arm.classIsProven) {
            IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(builder.appendTypeTest(iterable, arm.testedType), OperandIndex::TypeTest::value, BlockFrequency::Normal, BlockFrequency::Unlikely);
            appendElements.setSourceOperand(branch.provenOnTaken);
            builder.moveToEndOf(branch.notTaken);
            if (loopEntry)
                builder.emitJump(loopEntry, { branch.provenOnNotTaken });
            else {
                builder.append(makeRequireObjectCoercible(branch.provenOnNotTaken, IterationOrigin::Spread));
                builder.emitUnreachable();
            }
            builder.moveToEndOf(branch.taken);
        }
        if (!arm.namesAreProven) {
            RELEASE_ASSERT(loopEntry);
            ValueID subject = appendElements.sourceOperand();
            IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(builder.append(makePristineTest(subject, BuiltinClasses::Array, { PristineAspects::Storage, PristineAspects::Names })), 0, BlockFrequency::Normal, BlockFrequency::Unlikely);
            appendElements.setGuardOperand(branch.provenOnTaken);
            builder.moveToEndOf(branch.notTaken);
            builder.emitJump(loopEntry, { chooseArgumentOfProtocolEntry(pass, loopEntry, subject, iterable) });
            builder.moveToEndOf(branch.taken);
        }
        BlockID block = builder.currentBlock();
        checkAppendElements(block, builder.append(WTF::move(appendElements)));
        builder.endExpansion(cut, std::span<const ValueID> { });
    } else
        builder.emitJump(loopEntry, { iterable });

    if (!loopEntry)
        return;
    builder.moveToEndOf(loopEntry);
    ValueID open = builder.append(makeWith(Opcode::IteratorOpen, { source }, &Value::setIteratorKind, IteratorKind::Sync));
    builder.endExpansion(cut, std::span<const ValueID> { });
    loops.append({ open, original.arrayOperand(), loopEntry, cut.continuation, cut.handler, std::nullopt });
}

static std::optional<DerivedFunctionKey> findOutlinedLoop(FunctionPassContext& pass, const SpreadLoop& loop)
{
    if (isOutlinedIteration(pass.function) || pass.program.typeOf(pass.function, pass.function.at(loop.open).iterableOperand()).layouts)
        return std::nullopt;
    IterationPlan plan = planIteration(pass, Iteration { .open = loop.open, .origin = IterationOrigin::Spread });
    if (!plan.hasProtocolArm() || !plan.armSources.isEmpty())
        return std::nullopt;

    Vector<BuiltinClassID, maxExpectedClassesInKey> expectedClasses;
    for (const IterationArm& arm : plan.arms) {
        if (arm.builtinClass && arm.builtinClass != BuiltinClasses::Array && expectedClasses.size() < maxExpectedClassesInKey)
            expectedClasses.append(arm.builtinClass);
    }
    AOT::sort(expectedClasses, { }, &BuiltinClassID::index);
    return keyOfAppendIterable(expectedClasses.span());
}

static void replaceWithCallOfOutlinedLoop(FunctionPassContext& pass, IRBuilder& builder, const SpreadLoop& loop)
{
    Function& function = pass.function;
    RELEASE_ASSERT(function.at(loop.entry).values.size() == 1 && function.at(loop.entry).values[0] == loop.open);
    ValueID source = function.at(loop.open).iterableOperand();
    BlockFrequency frequency = function.at(loop.entry).frequency;
    builder.beginExpansion(ProgramPoint::atHeadOf(loop.entry));

    if (pass.program.typeOf(function, source).mayBeUndefinedOrNull()) {
        IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(builder.appendTypeTest(source, TypeTable::nullish), OperandIndex::TypeTest::value, BlockFrequency::Rare, frequency);
        builder.append(makeRequireObjectCoercible(branch.provenOnTaken, IterationOrigin::Spread));
        builder.emitUnreachable();
        builder.moveToEndOf(branch.notTaken);
        source = branch.provenOnNotTaken;
    }
    appendDirectCall(builder, { .callee = pass.derivedFunction(*loop.outlinedLoop), .arguments = std::array { loop.array, source }, .isNeverInlined = true });
    builder.emitJump(loop.continuation);
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
    Value iteratorResume = makeWith(Opcode::IteratorResume, { loop.open }, &Value::setIteratorKind, IteratorKind::Sync);
    iteratorResume.setResumeMode(ResumeMode::Next);
    ValueID step = builder.append(WTF::move(iteratorResume));
    ValueID done = builder.append(makeResultAt(step, doneResult));
    ValueID value = builder.append(makeResultAt(step, valueResult));
    IRBuilder::BranchBlocks blocks = builder.emitBranch(done, frequency, frequency);
    builder.emitJump(loop.continuation);

    builder.moveToEndOf(blocks.notTaken);
    builder.append(Opcode::AppendElement, { loop.array, value });
    builder.emitJump(header);
}

static PassOutcome expandSpreads(FunctionPassContext& pass, IRBuilder& builder, Vector<ValueID>& opensOfSpreads)
{
    Vector<std::pair<ProgramPoint, IterationPlan>> sites;
    forEachValueSite(pass.analyses, pass.function, { Opcode::AppendSpread }, [&](const ValueSite& site) {
        ValueID iterable = pass.function.at(site.value).iterableOperand();
        Arms arms = findClassArms(pass, PlannedValue { iterable, site.block, { PristineAspects::Storage, PristineAspects::Names } });
        // An arm's class is proven relative to the tests in front of it, so the other arms go before that is decided.
        arms.removeAllMatching([](const IterationArm& arm) { return arm.form != IterationForm::Array; });
        sites.append({ ProgramPoint { site.block, site.index }, planWithClassArms(pass, iterable, WTF::move(arms)) });
    });
    if (sites.isEmpty())
        return PassOutcome::Unchanged;

    AOT::sort(sites, std::ranges::greater { }, [](const auto& site) { return blockAndIndexOf(site.first); });
    Vector<SpreadLoop> loops;
    for (auto& [point, plan] : sites)
        expandSpread(pass, builder, point, plan, loops);

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
    builder.setHandler(std::nullopt);
    return PassOutcome::Changed;
}

// The passes.

// A step of a for..in stays in the function for good: once it has its position there is nothing left to do for it. A run that finds no work computes no analysis but the set of opcodes, which the next pass asks for too.
static OptionSet<ExpansionStep> findStepsWithWork(FunctionPassContext& pass)
{
    OptionSet<ExpansionStep> steps;
    if (!pass.analyses.opcodesOfValues().intersects(expandedOpcodes))
        return steps;
    for (const Block& block : pass.function.blocks) {
        for (ValueID id : block.values) {
            const Value& value = pass.function.at(id);
            if (value.opcode != Opcode::ForInStep || !value.positionOperand())
                steps.add(stepThatExpands(value.opcode));
        }
    }
    return steps;
}

static PassOutcome callGeneratorMethods(FunctionPassContext& pass, IRBuilder& builder)
{
    Function& function = pass.function;
    for (const Iteration& iteration : collectIterations(pass)) {
        ValueID iterable = function.at(iteration.open).iterableOperand();
        FunctionID method = iteration.kind == IteratorKind::Sync ? findGeneratorMethodOpening(pass, iterable) : FunctionID();
        if (!method)
            continue;

        auto insertBeforeOpen = [&, point = pass.analyses.useLists().pointOf(iteration.open)](auto&& value) {
            return builder.insertBefore(point, WTF::move(value));
        };
        InsertValue insert = insertBeforeOpen;
        ValueInsertion insertion { pass.analyses.context(), function, pass.types, function.at(iteration.open).origin(), insert };
        Value closure = makeWith(Opcode::NewClosure, { }, &Value::setFunction, method);
        closure.setIsHeldByClosureTable(true);
        closure.setOrigin(insertion.origin);
        ValueID generator = insert(makeDirectCall(insertion, { .callee = method, .thisValue = iterable, .calleeValue = insert(WTF::move(closure)) }));
        function.edit(iteration.open, ValueEdit::Definition).setOperand(OperandIndex::IteratorOpen::iterable, generator);
    }
    if (!builder.hasInsertions())
        return PassOutcome::Unchanged;
    builder.applyInsertions();
    return PassOutcome::Changed;
}

PassOutcome expandIteration(FunctionPassContext& pass)
{
    OptionSet<ExpansionStep> steps = findStepsWithWork(pass);
    if (steps.isEmpty())
        return PassOutcome::Unchanged;

    IRBuilder builder(pass.analyses.context(), pass.function, pass.types, &pass.analyses);
    PassOutcome outcome = builder.emptyUnreachableBlocks() ? PassOutcome::Changed : PassOutcome::Unchanged;
    if (steps.contains(ExpansionStep::ForInPositions))
        outcome |= threadForInPositions(pass, builder);
    if (steps.contains(ExpansionStep::ArrayIteratorSteps))
        outcome |= expandArrayIteratorSteps(pass, builder);

    Vector<ValueID> opensOfSpreads;
    if (steps.contains(ExpansionStep::Spreads))
        outcome |= expandSpreads(pass, builder, opensOfSpreads);
    if (steps.contains(ExpansionStep::Iterations))
        outcome |= callGeneratorMethods(pass, builder);
    if (steps.contains(ExpansionStep::Iterations) || !opensOfSpreads.isEmpty())
        outcome |= expandIterations(pass, builder, opensOfSpreads.span(), ExpandedIterations::All);
    return outcome;
}

// Until an iteration is expanded its steps are unknown code, to the function and, through its effect summary, to every caller.
PassOutcome expandProvenIterations(FunctionPassContext& pass)
{
    if (!findStepsWithWork(pass).contains(ExpansionStep::Iterations))
        return PassOutcome::Unchanged;

    IRBuilder builder(pass.analyses.context(), pass.function, pass.types, &pass.analyses);
    PassOutcome outcome = builder.emptyUnreachableBlocks() ? PassOutcome::Changed : PassOutcome::Unchanged;
    return outcome | expandIterations(pass, builder, { }, ExpandedIterations::OnlyProven);
}

// Uses of the key that a step of a for..in has just found.

static std::optional<KeyedOperation> keyedOperationOf(const Program& program, const Value& value)
{
    if (value.opcode == Opcode::ElemGet)
        return KeyedOperation { Opcode::EnumeratorGetByVal, value.objectOperand(), value.propertyOperand() };
    if (value.opcode == Opcode::ElemHas)
        return KeyedOperation { Opcode::Boolean, value.objectOperand(), value.propertyOperand() };
    if (value.opcode != Opcode::CallBuiltin)
        return std::nullopt;

    BuiltinCall call = builtinCallOf(program, value);
    if (call.form != BuiltinCallForm::Whole)
        return std::nullopt;
    BuiltinID builtin = program.builtins().at(call.overload).builtin;
    if (builtin != Builtins::ObjectHasOwn && builtin != Builtins::ObjectPrototypeHasOwnProperty)
        return std::nullopt;
    bool isMethod = builtin == Builtins::ObjectPrototypeHasOwnProperty;
    ValueID object = call.operandAt(isMethod ? BuiltinOperand::receiver() : BuiltinOperand::argument(0));
    ValueID key = call.operandAt(BuiltinOperand::argument(isMethod ? 0 : 1));
    if (!object || !key)
        return std::nullopt;
    return KeyedOperation { Opcode::Boolean, object, key };
}

void reportEnumeratedKeyTests(TestSiteReporter& reporter)
{
    const Function& function = reporter.function();
    if (!function.values.containsIf([](const Value& value) { return value.opcode == Opcode::ForInStep; }))
        return;
    auto isResultRead = [](const Value& value, ForInStepResult result) { return value.opcode == Opcode::ResultAt && value.index() == static_cast<uint32_t>(result); };

    reporter.forEachCandidate([&](const Value& use) {
        std::optional<KeyedOperation> operation = keyedOperationOf(reporter.program(), use);
        if (!operation)
            return;
        const Value& origin = function.at(reporter.analyses().identityOf(operation->key));
        if (!isResultRead(origin, ForInStepResult::Key) || function.at(origin.resultsOperand()).opcode != Opcode::ForInStep || !reporter.typeOf(operation->object).hasObjects())
            return;
        ValueID step = origin.resultsOperand();
        if (operation->opcodeWhenCurrent == Opcode::EnumeratorGetByVal && (reporter.program().expectedOrTypeOf(function, operation->object).hasOnlyLayouts(reporter.program()) || !reporter.typeOf(reporter.use()).isAnything()))
            return;

        for (const Use& read : reporter.analyses().useLists().usesOf(step)) {
            if (read.kind != Use::Kind::Operand || !isResultRead(function.at(read.user), ForInStepResult::Position) || !reporter.analyses().dominates(read.user, reporter.point()))
                continue;
            reporter.report({ IRBuilder::make(Opcode::IsEnumeratorCurrent, { function.at(step).enumeratorOperand(), operation->object, read.user }) }, declaredBoundOf(operation->opcodeWhenCurrent));
            return;
        }
    });
}

void rewriteTestedEnumeratedKeyUse(TestSiteRewriter& rewriter)
{
    FunctionPassContext& context = rewriter.context();
    const EffectContext& effectContext = context.analyses.effectContext();
    Value& use = rewriter.useValue();
    RELEASE_ASSERT(rewriter.tests().size() == 1 && rewriter.guard());
    const Value& test = rewriter.tests().front();
    KeyedOperation operation = *keyedOperationOf(rewriter.program(), use);

    ReplacedEffects replaced;
    replaced.add(effectContext, rewriter.function(), use);
    use.become(operation.opcodeWhenCurrent);
    if (operation.opcodeWhenCurrent == Opcode::Boolean)
        use.setBoolean(true);
    else {
        use.setObjectOperand(operation.object);
        use.setPropertyOperand(operation.key);
        use.setEnumeratorOperand(test.enumeratorOperand());
        use.setPositionOperand(test.positionOperand());
        use.setGuardOperand(rewriter.guard());
    }
    checkReplacement(effectContext, rewriter.function(), ReplacementSite { context.id, rewriter.point().block, rewriter.use() }, use, replaced, context.build.diagnostics());
    context.build.statistics().count(StatisticID::EnumeratedKeyUsesResolved);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
