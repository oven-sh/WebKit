/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRelationalSummariesInternal.h"

#if ENABLE(AOT)

#include "AOTAnalysisContext.h"
#include "AOTBuildContext.h"
#include "AOTEffectsDeclared.h"
#include "AOTFlowResults.h"
#include "AOTFunctionEditing.h"
#include "AOTIRBuilder.h"
#include "AOTResultRules.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include <wtf/DataLog.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

// One of the values that the result of a call may be, in a form that a test at run time decides.
struct Candidate {
    enum class Kind : uint8_t {
        // The value that the call passes.
        Operand,
        // A copy of the operation out of the callee.
        Literal,
        // All that is tested of an operator's result and of an object is its type.
        OfType,
    };

    Kind kind { Kind::Operand };
    ValueID operand;
    Value literal { Opcode::Undefined };
    Type type;
};

// What the table says that a member of the result holds when the call returns.
struct TestOfMember {
    PropertyKeyID key;
    Vector<Candidate, 4> candidates;
};

struct TestBehindCall {
    BlockID block;
    uint32_t indexInBlock { 0 };
    Vector<Candidate, 4> candidates;
    Vector<TestOfMember, 0> members;
    // Where the result is nothing but objects that were made for the call: the types of the operations that make them. typeResultsByOrigins() takes those for the type of the result.
    Vector<TypeID, 2> typesOfAllocations;
};

// Answers false where the origin is nothing that a test can decide.
bool appendCandidates(const Program& program, const Function& callee, const Value& call, const ResultOrigin& answered, uint32_t index, Vector<Candidate, 4>& candidates)
{
    const LinkedOrigin& origin = answered.origins[index];
    Candidate candidate;
    switch (origin.kind) {
    case LinkedOrigin::Kind::Parameter:
        if (std::optional<uint32_t> operand = findOperandPassedFor(callee, call, origin.parameter))
            candidate.operand = call.operand(*operand);
        else if (isParameterWithoutArgument(callee, call, origin.parameter))
            candidate.kind = Candidate::Kind::Literal;
        else
            return false;
        break;
    case LinkedOrigin::Kind::Constant:
        candidate.kind = Candidate::Kind::Literal;
        if (origin.value)
            candidate.literal = program.at(origin.function).at(origin.value);
        if (candidate.literal.opcode != origin.opcode)
            return false;
        break;
    case LinkedOrigin::Kind::Computed: {
        std::optional<Type> widest = tryWidestNaturalTypeOf(Value(origin.opcode));
        if (!widest)
            return false;
        candidate.kind = Candidate::Kind::OfType;
        candidate.type = WTF::move(*widest);
        break;
    }
    case LinkedOrigin::Kind::Anything:
        if (origin.type != TypeTable::object)
            return false;
        [[fallthrough]];
    case LinkedOrigin::Kind::Allocation:
        candidate.kind = Candidate::Kind::OfType;
        candidate.type = program.types.at(TypeTable::object);
        break;
    case LinkedOrigin::Kind::OneOf:
        for (uint32_t operand : answered.operands.span().subspan(origin.firstOperand, origin.operandCount)) {
            if (!appendCandidates(program, callee, call, answered, operand, candidates))
                return false;
        }
        return true;
    case LinkedOrigin::Kind::ReadFrom:
    case LinkedOrigin::Kind::BuiltinResult:
    case LinkedOrigin::Kind::CallResult:
        return false;
    }
    candidates.append(WTF::move(candidate));
    return true;
}

// Flow has counted the reads of a field before these tests are put in, and passes rely on what it found: a field that nobody reads is removed, and one whose value is only called or only looked into may hold something else than the value.
bool mayBeReadByAnybody(const Program& program, const Type& object, PropertyKeyID key)
{
    std::span<const LayoutID> layouts = membersOf(program, object);
    return !layouts.empty() && std::ranges::all_of(layouts, [&](LayoutID layout) {
        const auto& fields = program.at(layout).fields;
        auto field = std::ranges::find(fields, key, &Field::key);
        return field != fields.end() && field->flow.location.reads == ReadState::Unknown;
    });
}

// Of a result that is one object of which the table lists all stores: a member holds one of all that was stored under its name.
void appendTestsOfMembers(const Program& program, const Function& callee, const Value& call, const ResultOrigin& answered, Vector<TestOfMember, 0>& members)
{
    if (answered.mayBe.size() != 1)
        return;
    const LinkedOrigin& object = answered.origins[answered.mayBe[0]];
    if (object.kind != LinkedOrigin::Kind::Allocation || !object.hasOnlyListedStores)
        return;
    std::span<const StoreIntoOrigin> stores = answered.stores.span().subspan(object.firstStore, object.storeCount);
    if (std::ranges::any_of(stores, [](const StoreIntoOrigin& store) { return !store.key; }))
        return;
    for (const StoreIntoOrigin& first : stores) {
        if (program.at(first.key).kind != PropertyKeyKind::Text || std::ranges::find(members, first.key, &TestOfMember::key) != members.end())
            continue;
        if (!object.type || !mayBeReadByAnybody(program, program.types.at(object.type), first.key))
            continue;
        TestOfMember member { first.key, { } };
        bool isDecidable = true, isThere = false;
        for (const StoreIntoOrigin& store : stores) {
            if (store.key != first.key)
                continue;
            isThere |= store.isAtCreation || store.isOnEveryPath;
            isDecidable = isDecidable && appendCandidates(program, callee, call, answered, store.origin, member.candidates);
        }
        if (!isDecidable)
            continue;
        if (!isThere)
            member.candidates.append(Candidate { Candidate::Kind::Literal, { }, Value(Opcode::Undefined), { } });
        members.append(WTF::move(member));
    }
}

// None: one of the origins is nothing that a test can decide, or the table does not list the result.
std::optional<TestBehindCall> planTest(const Program& program, const FlowResults& flow, const Value& call)
{
    if (effectRuleOf(call.opcode) != EffectRule::Call || maySuspend(call.opcode))
        return std::nullopt;
    CalleeList callees = calleesOfCall(program, call);
    if (!callees.isComplete || !callees.builtins.isEmpty() || callees.functions.isEmpty())
        return std::nullopt;
    if (Value::opcodesWithCallees.contains(call.opcode) && (!Value::opcodesWithAccessSite.contains(call.opcode) || flow.mayCallBoundFunction(call.accessSite())))
        return std::nullopt;

    bool isConstruction = OperandPosition::newTarget(call.opcode).has_value();
    TestBehindCall test;
    unsigned numberOfCalleesEntered = 0;
    bool areAllAllocations = true;
    for (FunctionID id : callees.functions) {
        const Function& callee = program.at(id);
        if (throwsBeforeEntry(callee, isConstruction))
            continue;
        if (isConstruction && callee.kind == FunctionKind::Plain)
            return std::nullopt;
        ResultOrigin answered = flow.originOfResult(id, 0);
        if (!answered.isFullyListed)
            return std::nullopt;
        for (uint32_t root : answered.mayBe) {
            if (!appendCandidates(program, callee, call, answered, root, test.candidates))
                return std::nullopt;
            const LinkedOrigin& origin = answered.origins[root];
            areAllAllocations = areAllAllocations && origin.kind == LinkedOrigin::Kind::Allocation && origin.type;
            test.typesOfAllocations.append(origin.type);
        }
        // Which of several callees has made the object is not known behind the call.
        if (!numberOfCalleesEntered++)
            appendTestsOfMembers(program, callee, call, answered, test.members);
        else
            test.members.clear();
    }
    if (!areAllAllocations)
        test.typesOfAllocations.clear();
    return test;
}

Vector<TestBehindCall> planTests(const Program& program, const FlowResults& flow, const Function& function)
{
    Vector<TestBehindCall> tests;
    for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        const Block& block = function.blocks[blockIndex];
        for (uint32_t index = 0; index < block.values.size(); ++index) {
            // Nothing stands behind a tail call.
            if (block.terminator.kind == TerminatorKind::ReturnCall && block.terminator.callOperand() == block.values[index])
                continue;
            std::optional<TestBehindCall> test = planTest(program, flow, function.at(block.values[index]));
            if (!test)
                continue;
            test->block = BlockID(blockIndex);
            test->indexInBlock = index;
            tests.append(WTF::move(*test));
        }
    }
    return tests;
}

// `value === a | value === b | typeof value is ..`, and the process ends where none of them holds. NaN is not equal to itself, so it passes for whatever was listed.
void insertTest(IRBuilder& builder, ProgramPoint point, ValueID value, std::span<const Candidate> candidates)
{
    auto insert = [&](Value&& inserted) {
        return builder.insertBefore(point, WTF::move(inserted));
    };
    ValueID isListed = insert(IRBuilder::make(Opcode::LogicalNot, { insert(IRBuilder::make(Opcode::StrictEq, { value, value })) }));
    for (const Candidate& candidate : candidates) {
        ValueID isCandidate;
        switch (candidate.kind) {
        case Candidate::Kind::Operand:
            isCandidate = insert(IRBuilder::make(Opcode::StrictEq, { value, candidate.operand }));
            break;
        case Candidate::Kind::Literal:
            isCandidate = insert(IRBuilder::make(Opcode::StrictEq, { value, insert(Value(candidate.literal)) }));
            break;
        case Candidate::Kind::OfType: {
            Value typeTest = IRBuilder::make(Opcode::TypeTest, { value });
            typeTest.setTestedType(builder.types().intern(candidate.type));
            isCandidate = insert(WTF::move(typeTest));
            break;
        }
        }
        isListed = insert(IRBuilder::make(Opcode::BitOr, { isListed, isCandidate }));
    }
    Value yes = IRBuilder::make(Opcode::Boolean);
    yes.setBoolean(true);
    insert(IRBuilder::make(Opcode::ValidateSameValue, { insert(IRBuilder::make(Opcode::ToBoolean, { isListed })), insert(WTF::move(yes)) }));
}

void insertTests(IRBuilder& builder, const Function& function, const TestBehindCall& test)
{
    ValueID result = function.at(test.block).values[test.indexInBlock];
    ProgramPoint behindCall { test.block, test.indexInBlock + 1 };
    builder.setOrigin(function.at(result).origin());
    insertTest(builder, behindCall, result, test.candidates.span());
    if (!test.typesOfAllocations.isEmpty()) {
        Type type = Type::never();
        for (TypeID ofAllocation : test.typesOfAllocations)
            type = type.unionWith(builder.context().program.types.at(ofAllocation), builder.types());
        Value validation = IRBuilder::make(Opcode::ValidateType, { result });
        validation.setValidatedType(builder.types().intern(WTF::move(type)));
        builder.insertBefore(behindCall, WTF::move(validation));
    }
    for (const TestOfMember& member : test.members) {
        Value read = IRBuilder::make(Opcode::PropGet, { result });
        read.setKey(member.key);
        insertTest(builder, behindCall, builder.insertBefore(behindCall, WTF::move(read)), member.candidates.span());
    }
}

} // anonymous namespace

void validateResultOriginsAtRunTime(ProgramEditor& editor, const Program& program, const FlowResults& flow, BuildContext& build)
{
    // A literal is copied out of the callee's body, so nothing is written until all is read.
    const ParallelLoops& loops = build.parallelLoops();
    ParallelArray<Vector<TestBehindCall>> tests(loops, program.functions.size(), [&](unsigned index) {
        return planTests(program, flow, program.functions[index]);
    });
    Vector<FunctionID> functions;
    size_t numberOfTests = 0, numberOfMembers = 0;
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        if (tests[index].isEmpty())
            continue;
        functions.append(FunctionID(index));
        numberOfTests += tests[index].size();
        for (const TestBehindCall& test : tests[index])
            numberOfMembers += test.members.size();
    }
    if (build.options().logging.logsPhaseTimes) [[unlikely]]
        dataLogLn("AOT relational summaries: calls whose result is tested at run time ", numberOfTests, " | in functions ", functions.size(), " | members of results tested ", numberOfMembers);
    editFunctionsInParallel(editor, build, functions.span(), [&](unsigned, FunctionID id, Function& function, TaskArenas& arenas) {
        AnalysisContext context { build, program, nullptr, nullptr, id };
        IRBuilder builder(context, function, arenas.types);
        for (const TestBehindCall& test : tests[id.index()])
            insertTests(builder, function, test);
        builder.applyInsertions();
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
