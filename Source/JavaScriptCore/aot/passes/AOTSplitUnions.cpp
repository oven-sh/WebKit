/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSplitUnions.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinTable.h"
#include "AOTBuiltinUses.h"
#include "AOTCodeSizeEstimate.h"
#include "AOTEscapeAnalysis.h"
#include "AOTFunctionAnalyses.h"
#include "AOTHeat.h"
#include "AOTInt32Conversions.h"
#include "AOTMethodDispatch.h"
#include "AOTPropertyAccessElements.h"
#include "AOTRegionCloner.h"
#include "AOTResolveCalls.h"
#include "AOTSort.h"
#include "AOTTypeRelations.h"
#include "AOTValueSubstitution.h"
#include "AOTVector.h"
#include <algorithm>
#include <ranges>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct ChosenPart {
    const UnionPart* part { nullptr };
    bool isLikely { false };
    unsigned numberOfResolvedAccesses { 0 };
};

} // anonymous namespace

static std::optional<PropertyAccessQuery> accessQueryWithLimitOf(const FunctionPassContext& context, const Value& access)
{
    std::optional<PropertyAccessQuery> query = propertyAccessQueryOf(context.program, context.function, access);
    if (query)
        query->maxMembers = context.limit(LimitID::MaxAccessPlanMembers);
    return query;
}

static PropertyAccessPlan planNamedAccess(const FunctionPassContext& context, const PropertyAccessQuery& query)
{
    return planPropertyAccess({ context.program, &context.inputs.flow }, query);
}

static PropertyAccessPlan planWithReceivers(const FunctionPassContext& context, const PropertyAccessQuery& query, MemberStart start, ReceiverCandidates&& receivers)
{
    PropertyAccessQuery narrowed = query;
    narrowed.start = start;
    narrowed.receivers = WTF::move(receivers);
    return planNamedAccess(context, narrowed);
}

static PropertyAccessPlan planWithReceivers(const FunctionPassContext& context, const Value& access, MemberStart start, ReceiverCandidates&& receivers)
{
    std::optional<PropertyAccessQuery> query = accessQueryWithLimitOf(context, access);
    if (!query)
        return { };
    return planWithReceivers(context, *query, start, WTF::move(receivers));
}

static LayoutSetID receiversStampOf(const Value& access)
{
    return PayloadWord::receivers(access.opcode) ? access.receivers() : LayoutSetID();
}

static const Type* findExpectedType(const Program& program, const Function& function, ValueID id)
{
    TypeID expected = function.at(id).expected();
    return expected ? &program.at(expected) : nullptr;
}

static bool isAccessArmForm(AccessForm form)
{
    switch (form) {
    case AccessForm::Slot:
    case AccessForm::Constant:
    case AccessForm::Variable:
    case AccessForm::Namespace:
    case AccessForm::Builtin:
    case AccessForm::GetterInSlot:
        return true;
    case AccessForm::ByName:
    case AccessForm::Method:
    case AccessForm::Call:
    case AccessForm::Split:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isResolvableForm(ElementAccessForm form)
{
    switch (form) {
    case ElementAccessForm::Array:
    case ElementAccessForm::TypedArray:
    case ElementAccessForm::String:
    case ElementAccessForm::ArgumentsElement:
        return true;
    case ElementAccessForm::ByKey:
    case ElementAccessForm::NamedKey:
    case ElementAccessForm::Split:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool canResolveNamedAccess(const FunctionPassContext& context, const Value& access, const UnionPart& part)
{
    const Program& program = context.program;
    switch (part.kind) {
    case UnionPartKind::Layouts: {
        PropertyAccessPlan plan = planWithReceivers(context, access, MemberStart::Instance, candidatesOf(program, part.type, nullptr, receiversStampOf(access)));
        return plan.isResolved() || plan.form == AccessForm::Split;
    }
    case UnionPartKind::Functions: {
        LayoutID classLayout = classOfClassObject(program, part.type);
        if (!classLayout)
            return false;
        return planWithReceivers(context, access, MemberStart::ClassObject, { .proof = ReceiverProof::Type, .level = KnowledgeLevel::Proven, .proven = { classLayout } }).isResolved();
    }
    case UnionPartKind::Boolean:
    case UnionPartKind::Number:
    case UnionPartKind::String:
    case UnionPartKind::Symbol:
    case UnionPartKind::BigInt:
    case UnionPartKind::BuiltinClass:
    case UnionPartKind::Builtins: {
        BuiltinUseInputs inputs { program, context.function, context.types, context.inputs.flow, context.analyses.effectContext(), &context.analyses, { } };
        return resolutionWithReceiver(inputs, access, part.type) == BuiltinUseResolution::Resolves;
    }
    case UnionPartKind::OtherObjects:
    case UnionPartKind::Nullish:
    case UnionPartKind::Absent:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool canResolveElementAccess(const Program& program, const Function& function, const Value& access, const UnionPart& part)
{
    switch (part.kind) {
    case UnionPartKind::String:
    case UnionPartKind::BuiltinClass: {
        ValueID key = access.propertyOperand();
        ElementAccessPlan plan = planElementAccess(program, {
            .access = *elementAccessOf(access.opcode),
            .receiver = part.type,
            .receiverExpected = nullptr,
            .key = program.typeOf(function, key),
            .keyExpected = findExpectedType(program, function, key),
            .stored = OperandPosition::value(access.opcode) ? &program.typeOf(function, access.valueOperand()) : nullptr,
        });
        return isResolvableForm(plan.form);
    }
    case UnionPartKind::OtherObjects:
    case UnionPartKind::Nullish:
    case UnionPartKind::Boolean:
    case UnionPartKind::Number:
    case UnionPartKind::Symbol:
    case UnionPartKind::BigInt:
    case UnionPartKind::Absent:
    case UnionPartKind::Layouts:
    case UnionPartKind::Functions:
    case UnionPartKind::Builtins:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The getter in a slot is called behind a test of the slot, and nobody places a test in code that runs once: there the arm would read by name as the rest does.
static bool waitsForTestThatNobodyPlaces(FunctionPassContext& context, BlockID block, const Value& access, const UnionPart& part)
{
    if (part.kind != UnionPartKind::Layouts || !isNamedAccess(access.opcode) || heatClassOf(context, block) != HeatClass::RunsOnce)
        return false;
    return planWithReceivers(context, access, MemberStart::Instance, candidatesOf(context.program, part.type, nullptr, receiversStampOf(access))).form == AccessForm::GetterInSlot;
}

static std::optional<uint32_t> splitOperandOf(const Value& value)
{
    if (!accessesByNameOrKey.contains(value.opcode))
        return std::nullopt;
    if (isNamedAccess(value.opcode))
        return *receiverOf(value.opcode);
    if (elementAccessOf(value.opcode))
        return OperandPosition::object(value.opcode);
    return std::nullopt;
}

static bool isInRareBlock(FunctionPassContext& context, const ValueSite& site)
{
    return context.analyses.blockFrequencies().classOf(site.block) == BlockFrequency::Rare;
}

static bool resolvesOnPart(const FunctionPassContext& context, const Value& access, const UnionPart& part)
{
    if (isNamedAccess(access.opcode))
        return canResolveNamedAccess(context, access, part);
    return canResolveElementAccess(context.program, context.function, access, part);
}

AccessesResolvedOnParts countAccessesResolvedOnParts(FunctionPassContext& context, std::span<const ValueID> receivers, const UnionParts& parts)
{
    AccessesResolvedOnParts result;
    result.countOfPart.fill(0, parts.size());
    const BlockFrequencies& frequencies = context.analyses.blockFrequencies();
    for (ValueID receiver : receivers) {
        for (const Use& use : context.analyses.useLists().usesOf(receiver)) {
            if (use.kind != Use::Kind::Operand || frequencies.classOf(use.block) == BlockFrequency::Rare)
                continue;
            const Value& access = context.function.at(use.user);
            if (splitOperandOf(access) != use.operandIndex)
                continue;
            ++result.numberOfAccesses;
            bool isResolvable = false;
            for (size_t index = 0; index < parts.size(); ++index) {
                if (!resolvesOnPart(context, access, parts[index]))
                    continue;
                ++result.countOfPart[index];
                isResolvable = true;
            }
            result.numberOfResolvableAccesses += isResolvable;
            result.numberOfResolvableElementAccesses += isResolvable && !isNamedAccess(access.opcode);
        }
    }
    return result;
}

static bool isClassWithoutInstanceTest(const Program& program, const UnionPart& part)
{
    return part.kind == UnionPartKind::BuiltinClass && program.builtins().at(part.type.classes[0].builtinClass).instanceTest == BuiltinInstanceTest::None;
}

// The object of such a built-in is no constant of the realm, so a test for it never passes.
static bool hasUntestableBuiltin(const Program& program, const UnionPart& part)
{
    return std::ranges::any_of(part.type.builtins, [&](BuiltinID builtin) {
        return !isTestableBuiltin(program.builtins(), builtin);
    });
}

static Type testedTypeOf(const UnionPart& part)
{
    // A cast takes contents and storage kind from a tested type that names contents, so the bare class keeps what the value's own part says.
    return part.kind == UnionPartKind::BuiltinClass ? Type::builtinClass(part.type.classes[0].builtinClass).withoutClassProofs() : part.type;
}

// A class without an instance test overlaps every class, so a class is expected where the expected type names it.
static bool isExpected(const Program& program, const UnionPart& part, const Type& expected)
{
    if (part.kind == UnionPartKind::BuiltinClass)
        return expected.findClassPart(part.type.classes[0].builtinClass);
    return part.type.overlaps(expected, program);
}

static bool canSeparateByTest(const Program& program, TypeArena& types, const Type& whole, const UnionParts& parts, const UnionPart& part)
{
    if (hasUntestableBuiltin(program, part))
        return false;
    if (isClassWithoutInstanceTest(program, part)) {
        return std::ranges::none_of(parts, [&](const UnionPart& other) {
            return &other != &part && other.type.hasObjects();
        });
    }
    // Where another part passes the test too (a subclass of the tested class), the test narrows nothing, and the arm would be split again.
    return unionPartsOf(whole.castTo(testedTypeOf(part), types)).size() == 1;
}

static bool isArgumentsObject(const Type& type)
{
    const ClassPart* part = type.singleBuiltinClass();
    return part && part->builtinClass == BuiltinClasses::Arguments && !type.mayBeUndefinedOrNull();
}

static bool isReadByKey(const Function& function, const Use& use)
{
    return use.kind == Use::Kind::Operand && function.at(use.user).opcode == Opcode::ElemGet && use.operandIndex == OperandIndex::ElemGet::object;
}

// Read under a number, an arguments object that stays in its function is never allocated.
static bool needsCellForReadsByKeyOnly(FunctionAnalyses& analyses, ValueID arguments)
{
    const EscapeAnalysis& escape = analyses.escape();
    if (escape.gaveUp())
        return false;
    std::optional<ObjectID> object = escape.singleObject(arguments);
    if (!object || escape.at(*object).state == EscapeState::Unknown)
        return false;
    if (analyses.function().at(escape.at(*object).definition).opcode != Opcode::CreateUnmappedArguments)
        return false;
    return std::ranges::all_of(escape.usesOf(*object), [&](const ObjectUse& objectUse) {
        return !objectUse.isEscape || isReadByKey(analyses.function(), objectUse.use);
    });
}

namespace {

struct LoopSplit {
    BlockID header;
    Edge entry;
    Vector<BlockID> region;
    ValueID receiver;
    ValueID narrowestName;
    TypeID testedType;
    bool restTakesEveryValue { false };
};

// A loop that takes the elements of one object, which may be of several classes, gets a copy for one class. The class is tested once, in front of the loop.
class LoopSplitter {
    WTF_MAKE_NONCOPYABLE(LoopSplitter);
public:
    explicit LoopSplitter(FunctionPassContext& context)
        : m_context(context)
        , m_program(context.program)
        , m_function(context.function)
        , m_analyses(context.analyses)
    {
    }

    PassOutcome run();

private:
    std::optional<LoopSplit> findSplit();
    std::optional<LoopSplit> planFor(const NaturalLoop&);
    bool servesEveryAccess(ValueID receiver, const BitVector* isInLoop, const UnionPart&);
    bool isUsedAsNumber(ValueID element);
    bool holdsNumbersByUse(ValueID receiver, const BitVector& isInLoop);
    bool isExpectedToHoldNumbers(ValueID receiver, ValueID narrowestName, const BitVector& isInLoop);
    ValueID findNarrowestName(ValueID receiver, ProgramPoint front);
    TypeID chooseTestedType(ValueID receiver, ValueID narrowestName, const BitVector& isInLoop, BlockID front, bool& restTakesEveryValue);
    void split(const LoopSplit&);

    FunctionPassContext& m_context;
    const Program& m_program;
    Function& m_function;
    FunctionAnalyses& m_analyses;
    BitVector m_isFinishedHeader;
    unsigned m_numberOfSplitLoops { 0 };
};

PassOutcome LoopSplitter::run()
{
    while (std::optional<LoopSplit> plan = findSplit()) {
        if (m_context.exceeds(LimitID::MaxSeparatedLoopsPerFunction, m_numberOfSplitLoops + 1))
            break;
        split(*plan);
        ++m_numberOfSplitLoops;
    }
    return m_numberOfSplitLoops ? PassOutcome::Changed : PassOutcome::Unchanged;
}

// Outer loops come first: the test stands in front of the widest loop that does not change the object.
std::optional<LoopSplit> LoopSplitter::findSplit()
{
    const NaturalLoops& loops = m_analyses.naturalLoops();
    Vector<const NaturalLoop*, 8> candidates;
    for (unsigned index = 0; index < loops.numLoops(); ++index) {
        if (!m_isFinishedHeader.get(loops.loop(index).header().index()))
            candidates.append(&loops.loop(index));
    }
    AOT::stableSort(candidates, std::ranges::greater { }, &NaturalLoop::size);
    for (const NaturalLoop* loop : candidates) {
        m_isFinishedHeader.set(loop->header().index());
        if (std::optional<LoopSplit> plan = planFor(*loop))
            return plan;
    }
    return std::nullopt;
}

bool LoopSplitter::servesEveryAccess(ValueID receiver, const BitVector* isInLoop, const UnionPart& part)
{
    const BlockFrequencies& frequencies = m_analyses.blockFrequencies();
    for (BlockID block : m_analyses.cfg().reversePostOrder()) {
        if ((isInLoop && !isInLoop->get(block.index())) || frequencies.classOf(block) == BlockFrequency::Rare)
            continue;
        for (ValueID id : m_function.at(block).values) {
            const Value& access = m_function.at(id);
            std::optional<uint32_t> operandIndex = splitOperandOf(access);
            if (!operandIndex || m_analyses.identityOf(access.operand(*operandIndex)) != receiver)
                continue;
            if (!resolvesOnPart(m_context, access, part))
                return false;
        }
    }
    return true;
}

bool LoopSplitter::isUsedAsNumber(ValueID element)
{
    static constexpr OpcodeSet numericOperations = int32ConvertingOperations | OpcodeSet {
        Opcode::Sub, Opcode::Mul, Opcode::Div, Opcode::Mod, Opcode::Pow, Opcode::Negate, Opcode::Increment, Opcode::Decrement,
    };
    static constexpr OpcodeSet operationsOnTwoOfAKind { Opcode::Add, Opcode::Less, Opcode::LessEq, Opcode::Greater, Opcode::GreaterEq, Opcode::Eq, Opcode::StrictEq };
    for (const Use& use : m_analyses.useLists().usesOf(element)) {
        if (use.kind != Use::Kind::Operand)
            continue;
        const Value& user = m_function.at(use.user);
        if (numericOperations.contains(user.opcode))
            return true;
        if (operationsOnTwoOfAKind.contains(user.opcode) && m_program.expectedOrTypeOf(m_function, user.operand(!use.operandIndex)).isNumber())
            return true;
        if (user.opcode == Opcode::Upcast && isUsedAsNumber(use.user))
            return true;
    }
    return false;
}

bool LoopSplitter::holdsNumbersByUse(ValueID receiver, const BitVector& isInLoop)
{
    for (size_t block : isInLoop) {
        for (ValueID id : m_function.blocks[block].values) {
            const Value& access = m_function.at(id);
            if (!elementAccessOf(access.opcode) || m_analyses.identityOf(access.objectOperand()) != receiver)
                continue;
            if (OperandPosition::value(access.opcode) ? m_program.expectedOrTypeOf(m_function, access.valueOperand()).isNumber() : isUsedAsNumber(id))
                return true;
        }
    }
    return false;
}

bool LoopSplitter::isExpectedToHoldNumbers(ValueID receiver, ValueID narrowestName, const BitVector& isInLoop)
{
    TypeID expected = m_function.at(narrowestName).expected();
    if (!expected)
        return holdsNumbersByUse(receiver, isInLoop);
    const Type& type = m_context.types.at(expected);
    if (type.anyObject || type.anyFunction || type.layouts || !type.functions.isEmpty() || !type.builtins.isEmpty())
        return false;
    if (type.classes.size() != 1 || type.classes[0].builtinClass != BuiltinClasses::Array)
        return false;
    const Type& contents = m_context.types.at(type.classes[0].contentsAt(0));
    if (contents.isNumberOrNullish())
        return true;
    return contents.primitives & Primitive::Number && holdsNumbersByUse(receiver, isInLoop);
}

// What a test in front of the loop has proven of the object holds inside it.
ValueID LoopSplitter::findNarrowestName(ValueID receiver, ProgramPoint front)
{
    ValueID narrowest;
    for (uint32_t index = 0; index < m_function.values.size(); ++index) {
        ValueID name(index);
        if (m_analyses.identityOf(name) != receiver || !m_analyses.dominates(name, front))
            continue;
        if (!narrowest || m_program.typeOf(m_function, name).isSubsetOf(m_program.typeOf(m_function, narrowest), m_program))
            narrowest = name;
    }
    return narrowest;
}

TypeID LoopSplitter::chooseTestedType(ValueID receiver, ValueID narrowestName, const BitVector& isInLoop, BlockID front, bool& restTakesEveryValue)
{
    const Type& whole = m_program.typeOf(m_function, narrowestName);
    if (isClosedUnion(whole)) {
        UnionParts parts = unionPartsOf(whole);
        const UnionPart* last = nullptr;
        unsigned numberOfServingParts = 0;
        for (const UnionPart& part : parts) {
            if (!canSeparateByTest(m_program, m_context.types, whole, parts, part) || !servesEveryAccess(receiver, &isInLoop, part))
                continue;
            ++numberOfServingParts;
            last = &part;
        }
        if (numberOfServingParts < 2)
            return { };
        return m_context.types.intern(testedTypeOf(*last));
    }

    // Nobody could follow the object. What is asked only for what bytes have may be bytes as well as the array that it is taken for.
    if (!whole.anyObject || !m_context.build.recordedOptions().usesExpectedTypes() || !isExpectedToHoldNumbers(receiver, narrowestName, isInLoop))
        return { };
    UnionParts bytes = unionPartsOf(Type::builtinClass(BuiltinClasses::Uint8Array).withoutClassProofs());
    if (!servesEveryAccess(receiver, nullptr, bytes[0]))
        return { };
    TypeID testedType = m_context.types.intern(bytes[0].type);
    Value test = IRBuilder::make(Opcode::TypeTest, { narrowestName });
    test.setTestedType(testedType);
    if (m_analyses.findEarlierTest(test, front) == EarlierTest::Failed)
        return { };
    restTakesEveryValue = true;
    return testedType;
}

std::optional<LoopSplit> LoopSplitter::planFor(const NaturalLoop& loop)
{
    BlockID header = loop.header();
    if (m_analyses.blockFrequencies().classOf(header) != BlockFrequency::Normal || heatClassOf(m_context, header) != HeatClass::Hot)
        return std::nullopt;
    bool isLoopOfProgram = std::ranges::any_of(m_function.at(header).values, [&](ValueID value) {
        return m_function.at(value).opcode == Opcode::LoopHint;
    });
    if (!isLoopOfProgram)
        return std::nullopt;
    std::optional<Edge> entry = m_analyses.naturalLoops().findSingleEntry(loop);
    if (!entry)
        return std::nullopt;

    LoopSplit plan;
    plan.header = header;
    plan.entry = *entry;
    BitVector isInLoop(m_function.blocks.size());
    uint64_t bytes = 0;
    for (unsigned index = 0; index < loop.size(); ++index) {
        BlockID block = loop.at(index);
        isInLoop.set(block.index());
        plan.region.append(block);
        bytes += estimatedCodeBytes(m_program, m_function, m_function.at(block));
    }
    if (m_context.exceeds(LimitID::MaxBytesOfSeparatedLoop, bytes))
        return std::nullopt;

    const UseLists& uses = m_analyses.useLists();
    const BlockFrequencies& frequencies = m_analyses.blockFrequencies();
    Vector<ValueID, 4> receivers;
    for (BlockID block : plan.region) {
        if (frequencies.classOf(block) == BlockFrequency::Rare)
            continue;
        for (ValueID id : m_function.at(block).values) {
            const Value& access = m_function.at(id);
            if (!elementAccessOf(access.opcode))
                continue;
            ValueID receiver = m_analyses.identityOf(access.objectOperand());
            if (!isInLoop.get(uses.blockOf(receiver).index()) && !receivers.contains(receiver))
                receivers.append(receiver);
        }
    }
    for (ValueID receiver : receivers) {
        plan.narrowestName = findNarrowestName(receiver, pointBeforeTerminatorOf(m_function, entry->from));
        if (!plan.narrowestName)
            continue;
        plan.testedType = chooseTestedType(receiver, plan.narrowestName, isInLoop, entry->from, plan.restTakesEveryValue);
        if (!plan.testedType)
            continue;
        if (RegionCloner::checkCanDuplicate(m_analyses, plan.region.span()) != CloneCheck::CanClone)
            return std::nullopt;
        plan.receiver = receiver;
        return plan;
    }
    return std::nullopt;
}

void LoopSplitter::split(const LoopSplit& plan)
{
    BitVector isInLoop(m_function.blocks.size());
    for (BlockID block : plan.region)
        isInLoop.set(block.index());

    // Whatever else names the object in front of the loop is the same object inside it.
    Vector<ValueID, 4> names;
    Vector<Use, 16> usesInLoop;
    const UseLists& uses = m_analyses.useLists();
    for (uint32_t index = 0; index < m_function.values.size(); ++index) {
        ValueID name(index);
        if (m_analyses.identityOf(name) != plan.receiver || uses.locationOf(name).placement == ValuePlacement::Detached || isInLoop.get(uses.blockOf(name).index()))
            continue;
        size_t numberOfUsesBefore = usesInLoop.size();
        for (const Use& use : uses.usesOf(name)) {
            if (isInLoop.get(use.block.index()))
                usesInLoop.append(use);
        }
        if (usesInLoop.size() > numberOfUsesBefore)
            names.append(name);
    }

    IRBuilder builder({ m_context.build, m_program, &m_context.inputs.flow, &m_context.inputs.effects, m_context.id }, m_function, m_context.types, &m_analyses);
    builder.setOrigin(m_function.at(plan.entry.from).terminator.origin());
    BlockID front = builder.splitEdge(plan.entry);
    BlockFrequency frequency = m_function.at(front).frequency;
    Arguments arguments(m_function.at(front).terminator.next().arguments);
    m_function.edit(front, { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments }).terminator = Terminator { };
    builder.moveToEndOf(front);
    if (plan.restTakesEveryValue)
        builder.emitFailureOfPredictions(Target { plan.header, Arguments(arguments), { } });
    ValueID test = builder.appendTypeTest(plan.narrowestName, plan.testedType);
    IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(test, OperandIndex::TypeTest::value, frequency, frequency);

    auto narrows = [&](ValueID proven, ValueID name) {
        return m_program.typeOf(m_function, proven).isSubsetOf(m_program.typeOf(m_function, name), m_program);
    };
    RegionCloner cloner(builder, m_function);
    for (ValueID name : names) {
        if (narrows(branch.provenOnTaken, name))
            cloner.mapValue(name, branch.provenOnTaken);
    }
    cloner.cloneBlocks(plan.region.span(), [](ValueID, Value&) { });
    builder.moveToEndOf(branch.taken);
    builder.emitJump(cloner.cloneOf(plan.header), Arguments(arguments));
    builder.moveToEndOf(branch.notTaken);
    builder.emitJump(plan.header, WTF::move(arguments));
    if (!plan.restTakesEveryValue) {
        for (const Use& use : usesInLoop) {
            if (narrows(branch.provenOnNotTaken, usedValueOf(m_function, use)))
                replaceUse(m_function, use, branch.provenOnNotTaken);
        }
    }
    m_analyses.didChangeControlFlow();
    cloner.repairSSA(m_analyses);
    m_analyses.didChangeControlFlow();

    for (BlockID block : plan.region) {
        m_isFinishedHeader.set(block.index());
        m_isFinishedHeader.set(cloner.cloneOf(block).index());
    }
}

} // anonymous namespace

PassOutcome splitUnions(FunctionPassContext& context)
{
    bool didSplitLoops = context.analyses.naturalLoops().numLoops() && LoopSplitter(context).run() == PassOutcome::Changed;
    if (didSplitLoops) {
        retypeValues(context);
        resolveBuiltins(context);
        resolveAccesses(context);
    }
    PassOutcome outcome = UnionSplitter(context).run();
    if (!didSplitLoops)
        return outcome;
    retypeValues(context);
    context.keptAnalyses = { };
    return PassOutcome::Changed;
}

UnionSplitter::UnionSplitter(FunctionPassContext& context)
    : m_context(context)
    , m_program(context.program)
    , m_function(context.function)
    , m_builder({ context.build, context.program, &context.inputs.flow, &context.inputs.effects, context.id }, context.function, context.types, &context.analyses)
    , m_expansion(context, m_builder)
{
}

PassOutcome UnionSplitter::run()
{
    Vector<PlannedSplit> plans = planSplits();
    if (plans.isEmpty())
        return PassOutcome::Unchanged;

    for (const PlannedSplit& plan : plans | std::views::reverse)
        rewrite(plan);

    m_context.build.statistics().count(StatisticID::UnionsSplit, plans.size());
    // Each rewrite tells the analyses of its edits as it makes them.
    m_context.keptAnalyses = allAnalyses;
    return PassOutcome::Changed;
}

Vector<PlannedSplit> UnionSplitter::planSplits()
{
    Vector<PlannedSplit> plans;
    forEachValueSite(m_context.analyses, m_function, accessesByNameOrKey, [&](const ValueSite& site) {
        if (std::optional<PlannedSplit> plan = planSplit(site)) {
            moveToMemberCall(*plan);
            plans.append(WTF::move(*plan));
        }
    });

    const CFG& cfg = m_context.analyses.cfg();
    AOT::stableSort(plans, { }, [&](const PlannedSplit& plan) {
        return std::pair { cfg.reversePostOrderIndex(plan.site.block), plan.site.index };
    });
    return plans;
}

// Behind a join of the arms nobody knows any more which method belongs to which receiver. `x.m()` is split as a whole where the read may move down to the call.
void UnionSplitter::moveToMemberCall(PlannedSplit& plan)
{
    const Value& read = m_function.at(plan.site.value);
    if (read.opcode != Opcode::PropGet || plan.split.value != read.objectOperand())
        return;

    const UseLists& useLists = m_context.analyses.useLists();
    ValueID callee = plan.site.value;
    std::optional<Use> use = useLists.singleUse(callee);
    while (use && use->kind == Use::Kind::Operand && isCast(m_function.at(use->user).opcode) && !use->operandIndex) {
        callee = use->user;
        use = useLists.singleUse(callee);
    }
    if (!use || use->kind != Use::Kind::Operand || m_function.at(use->user).opcode != Opcode::Call || use->operandIndex != OperandIndex::Call::callee)
        return;

    const Value& call = m_function.at(use->user);
    if (call.thisValueOperand() != plan.split.value)
        return;
    ValueLocation location = useLists.locationOf(use->user);
    ValueSite callSite { location.block, location.index, use->user };
    MemberRead memberRead = findMemberRead(m_context, callSite);
    if (memberRead.isEmpty() || !memberRead.canSinkToCall || memberRead.read().value != plan.site.value)
        return;

    plan.site = callSite;
    plan.operandIndex = OperandIndex::Call::thisValue;
    plan.memberRead = WTF::move(memberRead.links);
}

std::optional<PlannedSplit> UnionSplitter::planSplit(const ValueSite& site)
{
    const Value& value = m_function.at(site.value);
    std::optional<uint32_t> operandIndex = splitOperandOf(value);
    if (!operandIndex)
        return std::nullopt;
    if (!isNamedAccess(value.opcode) && !isRecordedAsRare(m_function, site.block)) {
        if (std::optional<PlannedSplit> plan = planSplitOfSymbolKey(site))
            return plan;
    }
    // Nothing is proven about the objects of such a type, and testing every unknown value for its primitive parts would only grow the code.
    if (m_program.typeOf(m_function, value.operand(*operandIndex)).anyObject)
        return std::nullopt;
    if (isRecordedAsRare(m_function, site.block))
        return std::nullopt;

    if (!isNamedAccess(value.opcode)) {
        std::optional<ElementAccessPlan> plan = planElementAccessOf(m_program, m_function, value);
        if (!plan || (plan->form != ElementAccessForm::ByKey && plan->form != ElementAccessForm::Split) || isInRareBlock(m_context, site))
            return std::nullopt;
        if (plan->form == ElementAccessForm::ByKey)
            return planSplitByKeys(site);
        return planSplitByParts(site, *operandIndex);
    }

    std::optional<PropertyAccessQuery> query = accessQueryWithLimitOf(m_context, value);
    if (!query)
        return std::nullopt;
    const ReceiverCandidates& receivers = query->receivers;
    if (!receivers.hasOtherParts && receivers.proven.size() < 2)
        return std::nullopt;
    if (isInRareBlock(m_context, site))
        return std::nullopt;

    PropertyAccessPlan access = planNamedAccess(m_context, *query);
    switch (access.form) {
    case AccessForm::Split:
        if (receivers.hasOtherParts)
            return planSplitByParts(site, *operandIndex);
        return planSplitByArms(site, *query, access);
    case AccessForm::ByName:
        if (receivers.isProven())
            return std::nullopt;
        return planSplitByParts(site, *operandIndex);
    case AccessForm::Slot:
    case AccessForm::Constant:
    case AccessForm::Variable:
    case AccessForm::Namespace:
    case AccessForm::Builtin:
    case AccessForm::Method:
    case AccessForm::Call:
    case AccessForm::GetterInSlot:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

std::optional<PlannedSplit> UnionSplitter::planSplitByParts(const ValueSite& site, uint32_t operandIndex)
{
    const Value& value = m_function.at(site.value);
    ValueID receiver = value.operand(operandIndex);
    const Type& whole = m_program.typeOf(m_function, receiver);
    UnionParts parts = unionPartsOf(whole);
    if (parts.size() < 2)
        return std::nullopt;

    const Type& expected = m_program.expectedOrTypeOf(m_function, receiver);
    AccessesResolvedOnParts evidence = countAccessesResolvedOnParts(m_context, singleElementSpan(receiver), parts);
    Vector<ChosenPart, 4> chosen;
    for (size_t index = 0; index < parts.size(); ++index) {
        const UnionPart& part = parts[index];
        if (canSeparateByTest(m_program, m_context.types, whole, parts, part) && resolvesOnPart(m_context, value, part) && !waitsForTestThatNobodyPlaces(m_context, site.block, value, part))
            chosen.append(ChosenPart { &part, isExpected(m_program, part, expected), evidence.countOfPart[index] });
    }
    // What is expected is tested first, then the part that serves most of what the function does with the value.
    bool isAccessedByNumber = evidence.numberOfResolvableElementAccesses;
    AOT::stableSort(chosen, [&](const ChosenPart& a, const ChosenPart& b) {
        if (a.isLikely != b.isLikely)
            return a.isLikely;
        if (a.numberOfResolvedAccesses != b.numberOfResolvedAccesses)
            return a.numberOfResolvedAccesses > b.numberOfResolvedAccesses;
        return isAccessedByNumber && a.part->type.hasObjects() && !b.part->type.hasObjects();
    });
    if (m_context.exceeds(LimitID::MaxDispatchCases, chosen.size(), site.value))
        chosen.shrink(static_cast<size_t>(m_context.limit(LimitID::MaxDispatchCases)));
    if (chosen.isEmpty())
        return std::nullopt;

    BlockFrequency frequency = m_function.at(site.block).frequency;
    BlockFrequency unlikelyFrequency = rarerOf(frequency, BlockFrequency::Unlikely);
    LayoutSetID stamp = receiversStampOf(value);
    bool hasCompleteStamp = stamp && m_program.at(stamp).isComplete;

    PlannedSplit plan;
    plan.site = site;
    plan.operandIndex = operandIndex;
    plan.split.value = receiver;
    for (const ChosenPart& arm : chosen) {
        plan.split.arms.append(SplitArm { m_context.types.intern(testedTypeOf(*arm.part)), arm.isLikely ? frequency : unlikelyFrequency });
        bool stampsLayouts = arm.part->kind == UnionPartKind::Layouts && !hasCompleteStamp;
        plan.armReceivers.append(stampsLayouts ? arm.part->type.layouts : LayoutSetID());
    }
    plan.split.hasRest = chosen.size() < parts.size();
    // What is expected goes somewhere: where no arm takes it, the rest does.
    plan.split.restFrequency = chosen.first().isLikely ? unlikelyFrequency : frequency;
    return plan;
}

std::optional<PlannedSplit> UnionSplitter::planSplitByArms(const ValueSite& site, const PropertyAccessQuery& query, const PropertyAccessPlan& access)
{
    if (isCold(heatClassOf(m_context, site.block)))
        return std::nullopt;

    const Value& value = m_function.at(site.value);
    const MemberPartition& partition = access.partition;

    DispatchPlan dispatch = planDispatch(m_context, partition, query.receivers, site.value, [&](const MemberArm& arm) {
        ReceiverCandidates receivers { .proof = query.receivers.proof, .level = query.receivers.level, .proven = arm.layouts };
        return isAccessArmForm(planWithReceivers(m_context, query, query.start, WTF::move(receivers)).form);
    });
    if (dispatch.arms.isEmpty())
        return std::nullopt;

    PlannedSplit plan;
    plan.site = site;
    plan.operandIndex = *receiverOf(value.opcode);
    plan.split = createSplit(m_context, dispatch, partition, value.operand(plan.operandIndex), m_function.at(site.block).frequency);
    for (uint32_t armIndex : dispatch.arms)
        plan.armReceivers.append(m_context.types.completeLayoutSet(partition.arms[armIndex].layouts.span()));
    if (dispatch.hasRest())
        plan.restReceivers = m_context.types.completeLayoutSet(dispatch.rest.span());
    return plan;
}

// With one of its keys the access is an access by name.
std::optional<PlannedSplit> UnionSplitter::planSplitByKeys(const ValueSite& site)
{
    const Value& value = m_function.at(site.value);
    ValueID key = value.propertyOperand();
    const Type& keyType = m_program.typeOf(m_function, key);
    if (keyType.primitives & Primitive::Number)
        return planSplitOfNumberKey(site);
    bool mayBeNullish = keyType.mayBeUndefinedOrNull();
    if (!keyType.hasOnlyPrimitives(Primitive::LiteralString | Primitive::UndefinedOrNull) || keyType.stringLiterals.size() + mayBeNullish < 2)
        return std::nullopt;

    for (StringID text : keyType.stringLiterals) {
        ConstantKey constantKey = constantKeyOf(m_program, Type::stringLiteral(text));
        if (!constantKey)
            return std::nullopt;
        Value named = value;
        becomeNamedAccess(named, internKey(m_context.types, constantKey));
        std::optional<PropertyAccessQuery> query = accessQueryWithLimitOf(m_context, named);
        if (!query || !planNamedAccess(m_context, *query).isResolved())
            return std::nullopt;
    }
    if (m_context.exceeds(LimitID::MaxDispatchCases, keyType.stringLiterals.size(), site.value))
        return std::nullopt;

    PlannedSplit plan;
    plan.site = site;
    plan.operandIndex = *OperandPosition::property(value.opcode);
    plan.split.value = key;
    BlockFrequency frequency = m_function.at(site.block).frequency;
    plan.split.hasRest = mayBeNullish;
    plan.split.restFrequency = rarerOf(frequency, BlockFrequency::Unlikely);
    for (StringID text : keyType.stringLiterals) {
        plan.split.arms.append(SplitArm { m_context.types.intern(Type::stringLiteral(text)), frequency });
        plan.armReceivers.append(LayoutSetID());
    }
    return plan;
}

// `const { kState } = require(..)` may find nothing as far as anybody knows, where somebody may see the module before its body has ended: the key is the symbol or undefined.
// With the symbol the access is an access by name, whatever is known about the receiver: few layouts have a field under a symbol, so the name says what the receiver is expected to be.
std::optional<PlannedSplit> UnionSplitter::planSplitOfSymbolKey(const ValueSite& site)
{
    const Value& value = m_function.at(site.value);
    ValueID key = value.propertyOperand();
    const Type& keyType = m_program.typeOf(m_function, key);
    if (!keyType.mayBeUndefinedOrNull() || !keyType.hasOnlyPrimitives(Primitive::LiteralSymbol | Primitive::UndefinedOrNull) || keyType.symbolLiterals.size() != 1)
        return std::nullopt;

    BlockFrequency frequency = m_function.at(site.block).frequency;
    PlannedSplit plan;
    plan.site = site;
    plan.operandIndex = *OperandPosition::property(value.opcode);
    plan.split.value = key;
    plan.split.arms.append(SplitArm { m_context.types.intern(Type::symbolLiteral(keyType.symbolLiterals[0])), frequency });
    plan.armReceivers.append(LayoutSetID());
    plan.split.hasRest = true;
    plan.split.restFrequency = rarerOf(frequency, BlockFrequency::Unlikely);
    return plan;
}

// With a number the access reads or writes an element. What else the key may be names a property, and that is rare: the key of an element is undefined where it was read past the end.
std::optional<PlannedSplit> UnionSplitter::planSplitOfNumberKey(const ValueSite& site)
{
    const Value& value = m_function.at(site.value);
    ValueID key = value.propertyOperand();
    ValueID receiver = value.objectOperand();
    UnionParts parts = unionPartsOf(m_program.typeOf(m_function, key));
    auto number = std::ranges::find(parts, UnionPartKind::Number, &UnionPart::kind);
    if (parts.size() < 2 || number == parts.end())
        return std::nullopt;
    // A key that may be anything is tested for a number only where that saves an arguments object.
    const Type& receiverType = m_program.typeOf(m_function, receiver);
    if (!isClosedUnion(parts) && !isArgumentsObject(receiverType))
        return std::nullopt;

    ValueID stored = OperandPosition::value(value.opcode) ? value.valueOperand() : ValueID();
    const Type* storedType = stored == key ? &number->type : stored ? &m_program.typeOf(m_function, stored) : nullptr;
    ElementAccessPlan element = planElementAccess(m_program, {
        .access = *elementAccessOf(value.opcode),
        .receiver = receiverType,
        .receiverExpected = findExpectedType(m_program, m_function, receiver),
        .key = number->type,
        .keyExpected = nullptr,
        .stored = storedType,
        .storedExpected = stored && stored != key ? findExpectedType(m_program, m_function, stored) : nullptr,
    });
    if (!isResolvableForm(element.form))
        return std::nullopt;
    if (element.form == ElementAccessForm::ArgumentsElement && (value.opcode != Opcode::ElemGet || !needsCellForReadsByKeyOnly(m_context.analyses, receiver)))
        return std::nullopt;

    BlockFrequency frequency = m_function.at(site.block).frequency;
    PlannedSplit plan;
    plan.site = site;
    plan.operandIndex = *OperandPosition::property(value.opcode);
    plan.split.value = key;
    plan.split.arms.append(SplitArm { m_context.types.intern(number->type), frequency });
    plan.armReceivers.append(LayoutSetID());
    plan.split.hasRest = true;
    plan.split.restFrequency = rarerOf(frequency, BlockFrequency::Unlikely);
    return plan;
}

void UnionSplitter::rewrite(const PlannedSplit& plan)
{
    RELEASE_ASSERT(plan.split.hasRest || plan.split.arms.size() > 1);
    m_expansion.begin(plan.site);
    if (!plan.memberRead.isEmpty()) {
        Vector<Value, 2> copies;
        for (const ValueSite& link : plan.memberRead) {
            copies.append(m_function.at(link.value));
            becomeUndefined(m_function, link.value);
        }
        m_expansion.addReplaced(copies.first());
        m_expansion.setOperationsBeforeOriginal(WTF::move(copies), OperandIndex::Call::callee);
        m_context.analyses.didChangeValues();
    }
    emitSplit(m_expansion, plan.split,
        [&](uint32_t armIndex, ValueID narrowed) {
            m_expansion.finishArm(m_expansion.appendOriginal({ plan.operandIndex, narrowed, plan.armReceivers[armIndex], { }, true }));
        },
        [&](ValueID narrowed) {
            m_expansion.finishArm(m_expansion.appendOriginal({ plan.operandIndex, narrowed, plan.restReceivers, { }, true }));
        });
    m_expansion.end();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
