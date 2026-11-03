/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowUnobservedDefinitions.h"

#if ENABLE(AOT)

#include "AOTAnalysisContext.h"
#include "AOTBodyIndex.h"
#include "AOTBuildContext.h"
#include "AOTBuiltinIDs.h"
#include "AOTCFG.h"
#include "AOTEdgeResults.h"
#include "AOTFunctionAnalyses.h"
#include "AOTPredictedValues.h"
#include "AOTSchemaOperandIndexes.h"
#include "AOTSort.h"
#include "AOTSortedSet.h"
#include "AOTThreads.h"
#include "AOTTypeArena.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include <algorithm>
#include <optional>
#include <utility>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

using Keys = SortedSet<PropertyKeyID, 8>;

struct DefinitionOfUndefined {
    ValueID definition;
    PropertyKeyID key;
};

struct InitializerSummary {
    Vector<DefinitionOfUndefined, 8> definitionsOfUndefined;
    Keys definedKeys;
    Keys keysShownToOtherCode;
    Keys keysDefinedBeforeOthers;
    bool usesObjectOnlyInDefinitions { false };
    bool holdsItself { false };
};

struct InitializerCall {
    FunctionID initializer;
    LayoutID classLayout;
    std::optional<Keys> observedKeys;
    Keys keysObservedWhereObjectRefusesDefinition { };
};

// The blocks that no version of the function enters: nothing has the type of one of their parameters. In the generic version a check lets everything through,
// so behind checks a cast counts as what it is handed, any other operation as the most that it makes of any operands, and a test leaves of that what it leaves.
class BlocksNeverEntered {
public:
    BlocksNeverEntered(const Program& program, const Function& function, const CFG& cfg)
        : m_blocks(function.blocks.size())
    {
        PredictedValues predicted(function, cfg);
        bool hasParameterBehindCheck = false;
        forEachParameterThatNothingIs(function, [&](BlockID block, ValueID parameter) {
            if (predicted.isBehindCheck(parameter))
                hasParameterBehindCheck = true;
            else
                m_blocks.quickSet(block.index());
        });
        if (!hasParameterBehindCheck)
            return;

        ScratchTypeArena scratch(program.types);
        TypeArena& types = scratch;
        TypingContext context { program, function, types };
        Vector<TypeID> typesWithoutChecks(function.values.size());
        auto typeWithoutChecks = [&](ValueID value) {
            while (predicted.isBehindCheck(value) && isCast(function.at(value).opcode))
                value = function.at(value).operand(0);
            if (!predicted.isBehindCheck(value))
                return function.at(value).type;
            TypeID& type = typesWithoutChecks[value.index()];
            if (std::optional<Type> widest = type ? std::nullopt : tryWidestNaturalTypeOf(function.at(value)))
                type = types.intern(WTF::move(*widest));
            return type;
        };
        auto comparesWithValueBehindCheck = [&](const Value& test, uint32_t operandIndex) {
            for (uint32_t index = 0; index < test.numberOfOperands(); ++index) {
                if (index != operandIndex && predicted.isBehindCheck(test.operand(index)))
                    return true;
            }
            return false;
        };
        for (BlockID block : cfg.reversePostOrder()) {
            const TargetList& targets = function.at(block).terminator.targets;
            for (uint32_t targetIndex = 0; targetIndex < targets.size(); ++targetIndex) {
                if (!cfg.singleIncomingEdge(targets[targetIndex].block))
                    continue;
                const auto& parameters = function.at(targets[targetIndex].block).parameters;
                uint32_t firstParameter = firstEdgeResultParameter(function, block, targetIndex);
                for (uint32_t resultIndex = 0; firstParameter + resultIndex < parameters.size(); ++resultIndex) {
                    ValueID parameter = parameters[firstParameter + resultIndex];
                    if (!predicted.isBehindCheck(parameter))
                        continue;
                    std::optional<EdgeResult> result = tryGetEdgeResult(function, { block, targetIndex, resultIndex });
                    if (!result || result->kind != EdgeResultKind::NarrowedOperand || !result->test)
                        continue;
                    TypeID source = typeWithoutChecks(result->subject);
                    const Value& test = function.at(result->test);
                    if (source && !comparesWithValueBehindCheck(test, result->operandIndex))
                        typesWithoutChecks[parameter.index()] = types.intern(narrowTypeByTest(context, types.at(source), test, result->operandIndex, result->side));
                }
            }
        }
        forEachParameterThatNothingIs(function, [&](BlockID block, ValueID parameter) {
            TypeID type = typeWithoutChecks(parameter);
            if (type && types.at(type).isNever())
                m_blocks.quickSet(block.index());
        });
    }

    bool contains(BlockID block) const { return m_blocks.quickGet(block.index()); }

private:
    static void forEachParameterThatNothingIs(const Function& function, NOESCAPE const Invocable<void(BlockID, ValueID)> auto& functor)
    {
        for (uint32_t block = 0; block < function.blocks.size(); ++block) {
            for (ValueID parameter : function.blocks[block].parameters) {
                if (function.at(parameter).type == TypeTable::never)
                    functor(BlockID(block), parameter);
            }
        }
    }

    BitVector m_blocks;
};

constexpr AbstractHeapSet propertyHeaps {
    AbstractHeap::ObjectFields, AbstractHeap::NamedProperties, AbstractHeap::ShapeWords, AbstractHeap::OrderWords, AbstractHeap::Elements, AbstractHeap::ArrayStorage
};

bool mayShowObjectToOtherCode(const Effects& effects)
{
    return effects.mayThrowByRule() || effects.mayBeTerminated || effects.mayRunOtherCode() || effects.reads.containsAny(propertyHeaps);
}

bool handsOperandOnUntested(Opcode opcode)
{
    return isCastOrConversion(opcode) && !mayThrow(opcode);
}

uint32_t countOperands(const Instruction& instruction, const Invocable<bool(ValueID)> auto& matches)
{
    uint32_t count = 0;
    for (uint32_t index = 0; index < instruction.numberOfOperands(); ++index)
        count += matches(instruction.operand(index));
    return count;
}

std::optional<PropertyKeyID> keyOfFieldAccess(const Program& program, const Value& value)
{
    switch (value.opcode) {
    case Opcode::PropGet:
    case Opcode::PropSet:
    case Opcode::PropDefine:
        return value.key();
    case Opcode::PrivateGet:
    case Opcode::PrivateSet:
    case Opcode::PrivateDefine:
        return fieldAt(program, { value.layout(), value.field() }).key;
    default:
        return std::nullopt;
    }
}

InitializerSummary summarizeInitializer(const Program& program, const Function& initializer)
{
    InitializerSummary summary;
    std::optional<unsigned> thisIndex = initializer.thisParameterIndex();
    if (!initializer.hasBody() || !thisIndex)
        return summary;
    CFG cfg(initializer);
    if (cfg.hasHandlers() || cfg.hasCycle())
        return summary;

    BlocksNeverEntered blocksNeverEntered(program, initializer, cfg);
    ValueIdentities identities(initializer, cfg);
    ValueID object = identities.identityOf(initializer.at(entryBlock).parameters[*thisIndex]);
    auto isObject = [&](ValueID id) {
        return id && identities.identityOf(id) == object;
    };
    Vector<ValueID, 4> closuresCapturingObject;
    auto aliasesObject = [&](ValueID id) {
        return id && (isObject(id) || closuresCapturingObject.contains(identities.identityOf(id)));
    };
    // Definitions are only appended, so each is added to a set of keys once.
    size_t numberOfShownDefinitions = 0;
    auto showDefinitions = [&] {
        for (const DefinitionOfUndefined& definition : summary.definitionsOfUndefined.span().subspan(numberOfShownDefinitions))
            summary.keysShownToOtherCode.add(definition.key);
        numberOfShownDefinitions = summary.definitionsOfUndefined.size();
    };
    size_t numberOfDefinitionsBeforeOthers = 0;

    Keys keysDefinedTwice;
    for (BlockID block : cfg.reversePostOrder()) {
        if (blocksNeverEntered.contains(block))
            continue;
        for (ValueID id : initializer.at(block).values) {
            const Value& value = initializer.at(id);
            if (handsOperandOnUntested(value.opcode))
                continue;

            uint32_t usesOfObject = countOperands(value, aliasesObject);
            if (!usesOfObject) {
                if (mayShowObjectToOtherCode(declaredBoundOf(value.opcode)))
                    showDefinitions();
                continue;
            }
            if (value.opcode == Opcode::NewClosure) {
                closuresCapturingObject.append(identities.identityOf(id));
                continue;
            }
            if (value.opcode == Opcode::PrivateHas && usesOfObject == 1 && isObject(value.objectOperand()))
                continue;
            std::optional<uint8_t> receiver = receiverOf(value.opcode);
            bool definesProperty = receiver && (accessOf(value.opcode) == Access::Define || value.opcode == Opcode::ElemDefine) && isObject(value.operand(*receiver));
            bool storesAlias = definesProperty && aliasesObject(value.valueOperand());
            if (!definesProperty || usesOfObject != 1u + storesAlias)
                return summary;
            summary.holdsItself |= storesAlias;
            for (const DefinitionOfUndefined& definition : summary.definitionsOfUndefined.span().subspan(numberOfDefinitionsBeforeOthers))
                summary.keysDefinedBeforeOthers.add(definition.key);
            numberOfDefinitionsBeforeOthers = summary.definitionsOfUndefined.size();
            std::optional<PropertyKeyID> key = keyOfFieldAccess(program, value);
            if (!key) {
                if (program.typeOf(initializer, value.operand(OperandIndex::ElemDefine::property)).hasObjects())
                    showDefinitions();
                continue;
            }
            if (!summary.definedKeys.add(*key))
                keysDefinedTwice.add(*key);
            if (initializer.at(value.valueOperand()).opcode == Opcode::Undefined)
                summary.definitionsOfUndefined.append({ id, *key });
        }

        bool terminatorUsesObject = false;
        initializer.at(block).terminator.forEachChild([&](ValueID child) {
            terminatorUsesObject |= aliasesObject(child);
        });
        if (terminatorUsesObject)
            return summary;
    }

    summary.definitionsOfUndefined.removeAllMatching([&](const DefinitionOfUndefined& definition) {
        return keysDefinedTwice.contains(definition.key);
    });
    summary.usesObjectOnlyInDefinitions = true;
    return summary;
}

struct ConstructionWalk {
    struct State {
        Keys storedKeys;
        bool holdsItself { false };
    };

    std::optional<Keys> findObservedKeys(uint32_t firstIndex)
    {
        walk(firstBlock, firstIndex, { { }, summary.holdsItself });
        while (std::exchange(hasChanged, false)) {
            for (BlockID block : analyses.cfg().reversePostOrder()) {
                if (block != firstBlock && statesAtHead[block.index()])
                    walk(block, 0, *statesAtHead[block.index()]);
            }
        }
        return entersFirstBlockAgain ? std::nullopt : std::optional(WTF::move(observedKeys));
    }

    void walk(BlockID block, uint32_t firstIndex, State state)
    {
        const BlockValues& values = function.at(block).values;
        for (uint32_t index = firstIndex; index < values.size(); ++index)
            visit(values[index], state);

        const Terminator& terminator = function.at(block).terminator;
        if (usesOfObjectIn(terminator) || leavesConstructor(terminator.kind) || mergesObjectWithOtherValues(terminator))
            observeEverything(state);
        for (const Target& target : terminator.targets) {
            if (blocksNeverEntered.contains(target.block))
                continue;
            if (target.block == firstBlock)
                entersFirstBlockAgain = true;
            else
                meet(statesAtHead[target.block.index()], state);
        }
    }

    void meet(std::optional<State>& atHead, const State& arriving)
    {
        if (!atHead) {
            atHead = arriving;
            hasChanged = true;
            return;
        }
        size_t numberOfStoredKeys = atHead->storedKeys.size();
        bool heldItself = atHead->holdsItself;
        atHead->storedKeys.intersect(arriving.storedKeys);
        atHead->holdsItself |= arriving.holdsItself;
        hasChanged |= atHead->storedKeys.size() != numberOfStoredKeys || atHead->holdsItself != heldItself;
    }

    bool isObject(ValueID id) { return id && analyses.identityOf(id) == object; }

    uint32_t usesOfObjectIn(const Instruction& instruction)
    {
        return countOperands(instruction, [&](ValueID operand) {
            return isObject(operand);
        });
    }

    bool callIsFirstUseOfNewObject(uint32_t indexOfCall)
    {
        const BlockValues& values = function.at(firstBlock).values;
        size_t allocation = values.find(object);
        if (function.at(object).opcode != Opcode::NewObject || allocation == notFound || allocation > indexOfCall)
            return false;
        for (size_t earlier = allocation + 1; earlier < indexOfCall; ++earlier) {
            if (usesOfObjectIn(function.at(values[earlier])) && !handsOperandOnUntested(function.at(values[earlier]).opcode))
                return false;
        }
        return true;
    }

    void observe(const State& state, PropertyKeyID key)
    {
        if (!state.storedKeys.contains(key))
            observedKeys.add(key);
    }

    void observeEverything(const State& state)
    {
        for (const DefinitionOfUndefined& definition : summary.definitionsOfUndefined)
            observe(state, definition.key);
    }

    void visit(ValueID id, State& state)
    {
        const Value& value = function.at(id);
        if (handsOperandOnUntested(value.opcode) || value.opcode == Opcode::TypeTest)
            return;
        if (!usesOfObjectIn(value)) {
            if (isKnownToOtherCode && mayShowObjectToOtherCode(analyses.effectsOf(id).onAnyEdge()))
                observeEverything(state);
            return;
        }
        std::optional<PropertyKeyID> key = keyOfFieldAccess(program, value);
        if (!key || !isObject(value.objectOperand()) || !summary.definedKeys.contains(*key) || (accessOf(value.opcode) == Access::Read && state.holdsItself)) {
            observeEverything(state);
            return;
        }
        if (accessOf(value.opcode) == Access::Read) {
            observe(state, *key);
            return;
        }
        state.holdsItself |= isObject(value.valueOperand());
        state.storedKeys.add(*key);
    }

    bool leavesConstructor(TerminatorKind kind) const
    {
        switch (kind) {
        case TerminatorKind::Return:
        case TerminatorKind::ReturnCall:
            return true;
        case TerminatorKind::Throw:
        case TerminatorKind::Rethrow:
            return isKnownToOtherCode;
        case TerminatorKind::Jump:
        case TerminatorKind::Branch:
        case TerminatorKind::Switch:
        case TerminatorKind::SwitchString:
        case TerminatorKind::Unreachable:
        case TerminatorKind::SwitchOnShape:
        case TerminatorKind::TryCallInline:
        case TerminatorKind::TestAndBranch:
            return false;
        }
    }

    bool mergesObjectWithOtherValues(const Terminator& terminator)
    {
        for (const Target& target : terminator.targets) {
            if (blocksNeverEntered.contains(target.block))
                continue;
            const auto& parameters = function.at(target.block).parameters;
            for (uint32_t index = 0; index < target.arguments.size(); ++index) {
                if (isObject(target.arguments[index]) && !isObject(parameters[index]))
                    return true;
            }
        }
        return false;
    }

    const Program& program;
    FunctionAnalyses& analyses;
    const InitializerSummary& summary;
    BlockID firstBlock;
    ValueID object;
    bool isKnownToOtherCode { true };
    const Function& function { analyses.function() };
    BlocksNeverEntered blocksNeverEntered { program, function, analyses.cfg() };
    Vector<std::optional<State>> statesAtHead = Vector<std::optional<State>>(function.blocks.size());
    Keys observedKeys { };
    bool hasChanged { false };
    bool entersFirstBlockAgain { false };
};

bool returnsOnlyConstructedObject(const Program& program, BuildContext& build, FunctionID id)
{
    const Function& constructor = program.at(id);
    if (!constructor.hasBody() || (constructor.kind != FunctionKind::BaseConstructor && constructor.kind != FunctionKind::DerivedConstructor && constructor.kind != FunctionKind::Plain))
        return false;

    FunctionAnalyses analyses(AnalysisContext { build, program, nullptr, nullptr, id }, constructor);
    std::optional<unsigned> thisIndex = constructor.thisParameterIndex();
    ValueID thisParameter = thisIndex ? analyses.identityOf(constructor.at(entryBlock).parameters[*thisIndex]) : ValueID();
    Vector<BlockID> blocks(analyses.cfg().reversePostOrder());
    BlocksNeverEntered blocksNeverEntered(program, constructor, analyses.cfg());
    blocks.removeAllMatching([&](BlockID block) {
        return blocksNeverEntered.contains(block);
    });
    Vector<ValueID, 8> worklist;
    for (BlockID block : blocks) {
        const Terminator& terminator = constructor.at(block).terminator;
        if (terminator.kind == TerminatorKind::ReturnCall)
            return false;
        if (terminator.kind == TerminatorKind::Return && terminator.valueOperand())
            worklist.append(terminator.valueOperand());
    }

    std::optional<PredictedValues> predicted;
    auto isNeverObject = [&](ValueID value) {
        if (program.typeOf(constructor, value).hasObjects())
            return false;
        if (!predicted)
            predicted.emplace(constructor, analyses.cfg(), PredictedValues::Options { .programToReadComparisonsWith = &program });
        return !predicted->isBehindCheck(value);
    };
    BitVector isVisited(constructor.values.size());
    while (!worklist.isEmpty()) {
        ValueID returned = analyses.identityOf(worklist.takeLast());
        if (isVisited.set(returned.index()) || returned == thisParameter || isNeverObject(returned))
            continue;
        const Value& value = constructor.at(returned);
        if (value.opcode == Opcode::SuperConstruct || value.opcode == Opcode::SuperConstructList || (value.opcode == Opcode::NewObject && program.at(value.layout()).kind == LayoutKind::Instance))
            continue;
        if (value.opcode != Opcode::Parameter)
            return false;
        bool isMerge = false;
        for (BlockID predecessor : blocks) {
            for (const Target& target : constructor.at(predecessor).terminator.targets) {
                size_t position = constructor.at(target.block).parameters.find(returned);
                if (position == notFound)
                    continue;
                if (position >= target.arguments.size())
                    return false;
                worklist.append(target.arguments[position]);
                isMerge = true;
            }
        }
        if (!isMerge)
            return false;
    }
    return true;
}

bool superReturnsNewObject(const Program& program, BuildContext& build, LayoutID classLayout)
{
    LayoutID current = classLayout;
    for (size_t steps = 0; steps < program.layouts.size() && program.at(current).has(LayoutFlags::HasHeritage); ++steps) {
        current = predictedHeritageOf(program.at(current));
        if (!current || !program.at(current).constructor || !returnsOnlyConstructedObject(program, build, program.at(current).constructor))
            return false;
    }
    return !program.at(current).has(LayoutFlags::HasHeritage);
}

bool keepsObjectToItself(const Program& program, BuildContext& build, FunctionID id, Keys& definedKeys)
{
    const Function& constructor = program.at(id);
    FunctionAnalyses analyses(AnalysisContext { build, program, nullptr, nullptr, id }, constructor);
    if (analyses.cfg().hasHandlers())
        return false;

    Vector<ValueID, 2> objects;
    if (std::optional<unsigned> thisIndex = constructor.thisParameterIndex())
        objects.append(analyses.identityOf(constructor.at(entryBlock).parameters[*thisIndex]));
    for (BlockID block : analyses.cfg().reversePostOrder()) {
        for (ValueID value : constructor.at(block).values) {
            Opcode opcode = constructor.at(value).opcode;
            if (opcode == Opcode::SuperConstruct || opcode == Opcode::SuperConstructList || (opcode == Opcode::NewObject && program.at(constructor.at(value).layout()).kind == LayoutKind::Instance))
                objects.append(analyses.identityOf(value));
        }
    }
    auto isObject = [&](ValueID operand) {
        return operand && objects.contains(analyses.identityOf(operand));
    };
    for (bool changed = true; std::exchange(changed, false);) {
        for (BlockID block : analyses.cfg().reversePostOrder()) {
            for (const Target& target : constructor.at(block).terminator.targets) {
                const auto& parameters = constructor.at(target.block).parameters;
                for (uint32_t index = 0; index < target.arguments.size(); ++index) {
                    if (isObject(target.arguments[index]) && !isObject(parameters[index])) {
                        objects.append(analyses.identityOf(parameters[index]));
                        changed = true;
                    }
                }
            }
        }
    }

    Vector<ValueID, 8> accesses;
    BlocksNeverEntered blocksNeverEntered(program, constructor, analyses.cfg());
    for (BlockID block : analyses.cfg().reversePostOrder()) {
        if (blocksNeverEntered.contains(block))
            continue;
        for (ValueID valueID : constructor.at(block).values) {
            const Value& value = constructor.at(valueID);
            if (handsOperandOnUntested(value.opcode) || value.opcode == Opcode::TypeTest || !countOperands(value, isObject))
                continue;
            if (value.opcode == Opcode::CallDirect && program.at(value.function()).kind == FunctionKind::Initializer) {
                const Function& initializer = program.at(value.function());
                InitializerSummary summary = summarizeInitializer(program, initializer);
                if (!summary.usesObjectOnlyInDefinitions || countOperands(value, isObject) != 1)
                    return false;
                unsigned thisIndex = *initializer.thisParameterIndex();
                if (thisIndex >= value.argumentsOperands().size() || !isObject(value.argumentsOperands()[thisIndex]))
                    return false;
                for (PropertyKeyID key : summary.definedKeys.span())
                    definedKeys.add(key);
                continue;
            }
            if (!keyOfFieldAccess(program, value) || !isObject(value.objectOperand()))
                return false;
            accesses.append(valueID);
        }

        const Terminator& terminator = constructor.at(block).terminator;
        if (terminator.kind != TerminatorKind::Return && countOperands(terminator, isObject))
            return false;
    }
    return std::ranges::all_of(accesses, [&](ValueID access) {
        return definedKeys.contains(*keyOfFieldAccess(program, constructor.at(access)));
    });
}

bool takesEveryDefinitionBehindSuper(const Program& program, BuildContext& build, LayoutID classLayout)
{
    Vector<LayoutID, 4> classesAbove;
    LayoutID current = predictedHeritageOf(program.at(classLayout));
    for (size_t steps = 0; current && steps < program.layouts.size(); ++steps) {
        classesAbove.append(current);
        current = predictedHeritageOf(program.at(current));
    }
    Keys definedKeys;
    for (size_t index = classesAbove.size(); index--;) {
        FunctionID constructor = program.at(classesAbove[index]).constructor;
        if (!constructor || !program.at(constructor).hasBody() || !keepsObjectToItself(program, build, constructor, definedKeys))
            return false;
    }
    return true;
}

bool isDeclaredByBaseClass(const Program& program, LayoutID classLayout, PropertyKeyID key)
{
    LayoutID current = predictedHeritageOf(program.at(classLayout));
    for (size_t steps = 0; current && steps < program.layouts.size(); ++steps) {
        if (program.at(current).findField(key))
            return true;
        current = predictedHeritageOf(program.at(current));
    }
    return false;
}

InitializerCall analyzeCall(const Program& program, BuildContext& build, FunctionID callerID, BlockID block, uint32_t index)
{
    const Function& caller = program.at(callerID);
    const Value& call = caller.at(caller.at(block).values[index]);
    const Function& initializer = program.at(call.function());
    InitializerCall result { call.function(), caller.constructedClass, std::nullopt };

    bool isConstructor = caller.kind == FunctionKind::BaseConstructor || caller.kind == FunctionKind::DerivedConstructor;
    if (!isConstructor || !caller.constructedClass)
        return result;
    InitializerSummary summary = summarizeInitializer(program, initializer);
    if (!summary.usesObjectOnlyInDefinitions || summary.definitionsOfUndefined.isEmpty())
        return result;
    unsigned thisIndex = *initializer.thisParameterIndex();
    if (thisIndex >= call.argumentsOperands().size())
        return result;

    FunctionAnalyses analyses(AnalysisContext { build, program, nullptr, nullptr, callerID }, caller);
    if (analyses.cfg().hasHandlers())
        return result;
    ConstructionWalk walk { program, analyses, summary, block, analyses.identityOf(call.argumentsOperands()[thisIndex]) };
    walk.isKnownToOtherCode = !walk.callIsFirstUseOfNewObject(index);
    if (walk.isKnownToOtherCode && !superReturnsNewObject(program, build, caller.constructedClass))
        return result;

    result.observedKeys = walk.findObservedKeys(index + 1);
    if (!result.observedKeys)
        return result;
    if (walk.isKnownToOtherCode) {
        result.observedKeys->unite(summary.keysShownToOtherCode);
        bool isEveryEarlierKeyObserved = std::ranges::all_of(summary.keysDefinedBeforeOthers.span(), [&](PropertyKeyID key) {
            return result.observedKeys->contains(key);
        });
        if (!isEveryEarlierKeyObserved && !takesEveryDefinitionBehindSuper(program, build, caller.constructedClass))
            result.keysObservedWhereObjectRefusesDefinition = summary.keysDefinedBeforeOthers;
    }
    for (const DefinitionOfUndefined& definition : summary.definitionsOfUndefined) {
        if (isDeclaredByBaseClass(program, caller.constructedClass, definition.key))
            result.observedKeys->add(definition.key);
    }
    return result;
}

} // anonymous namespace

static bool runsNoCodeAtHeadOfBody(const Value& value)
{
    switch (value.opcode) {
    case Opcode::NewClosure:
    case Opcode::NewObject:
    case Opcode::FinishObject:
    case Opcode::PutEnvironmentField:
        return true;
    default:
        return handsOperandOnUntested(value.opcode) || isRematerializable(value);
    }
}

// The files that a `require` finds run to their end wherever it stands: each request for them says so, none is an import, which is linked before the file has begun, and no way leads from the file back to it.
// A file between two cycles counts as on one.
static BitVector findFilesThatAreSeenWhole(const Program& program)
{
    size_t numberOfModules = program.modules.size();
    BitVector isSeenWhole(numberOfModules);
    Vector<uint32_t> numberOfTargets(FillWith { }, numberOfModules, 0u);
    Vector<uint32_t> numberOfReferrers(FillWith { }, numberOfModules, 0u);
    for (size_t index = 0; index < numberOfModules; ++index) {
        isSeenWhole.quickSet(index);
        numberOfTargets[index] = program.modules[index].requests.size();
        for (const ModuleRequest& request : program.modules[index].requests)
            ++numberOfReferrers[request.target.index()];
    }
    Vector<uint32_t> firstReferrer(numberOfModules + 1);
    uint32_t numberOfRequests = 0;
    for (size_t index = 0; index < numberOfModules; ++index)
        firstReferrer[index] = std::exchange(numberOfRequests, numberOfRequests + numberOfReferrers[index]);
    firstReferrer[numberOfModules] = numberOfRequests;
    Vector<uint32_t> referrers(numberOfRequests);
    Vector<uint32_t> nextReferrer(firstReferrer.span().first(numberOfModules));
    for (size_t index = 0; index < numberOfModules; ++index) {
        for (const ModuleRequest& request : program.modules[index].requests)
            referrers[nextReferrer[request.target.index()]++] = index;
    }
    BitVector liesOnNoCycle(numberOfModules);
    Vector<uint32_t> found;
    auto find = [&](uint32_t index) {
        if (!liesOnNoCycle.quickSet(index))
            found.append(index);
    };
    for (uint32_t index = 0; index < numberOfModules; ++index) {
        if (!numberOfTargets[index] || !numberOfReferrers[index])
            find(index);
    }
    while (!found.isEmpty()) {
        uint32_t index = found.takeLast();
        for (const ModuleRequest& request : program.modules[index].requests) {
            if (!--numberOfReferrers[request.target.index()])
                find(request.target.index());
        }
        for (uint32_t referrer : referrers.span().subspan(firstReferrer[index], firstReferrer[index + 1] - firstReferrer[index])) {
            if (!--numberOfTargets[referrer])
                find(referrer);
        }
    }
    isSeenWhole.filter(liesOnNoCycle);
    for (const Module& module : program.modules) {
        for (const ModuleRequest& request : module.requests) {
            if (request.kind != ModuleRequestKind::Require || request.isHoisted || !request.completes)
                isSeenWhole.quickClear(request.target.index());
        }
    }
    return isSeenWhole;
}

static BitVector findVariablesStoredAheadOfEveryRead(const Program& program, BuildContext& build)
{
    // Ahead of the store at the head of a body nothing has run, so nobody has a function of the file in hand. A store further down the one way that the body begins with is seen by whoever comes behind the end of the body.
    BitVector storedAtHead(program.variables.size());
    BitVector stored(program.variables.size());
    BitVector readAheadOfStore(program.variables.size());
    std::optional<BitVector> filesSeenWhole;
    constexpr OpcodeSet reads { Opcode::GetModuleVariable, Opcode::GetEncodedModuleVariable };
    for (uint32_t index = 0; index < program.modules.size(); ++index) {
        const Module& module = program.modules[index];
        if (module.kind != ModuleKind::CommonJs || !module.body || !program.at(module.body).hasBody() || program.at(module.body).module != ModuleID(index))
            continue;
        const Function& body = program.at(module.body);
        BlockID block = entryBlock;
        bool isAtHead = true;
        for (size_t steps = 0; steps < body.blocks.size() && !body.at(block).handler; ++steps) {
            for (ValueID id : body.at(block).values) {
                const Value& value = body.at(id);
                if (reads.contains(value.opcode) && program.at(value.variable()).module == ModuleID(index) && !stored.quickGet(value.variable().index()))
                    readAheadOfStore.quickSet(value.variable().index());
                if (value.opcode != Opcode::PutModuleVariable) {
                    isAtHead = isAtHead && runsNoCodeAtHeadOfBody(value);
                    continue;
                }
                const Variable& variable = program.at(value.variable());
                if (variable.module != ModuleID(index) || variable.initialization != VariableInitialization::Undefined)
                    continue;
                if (isAtHead)
                    storedAtHead.quickSet(value.variable().index());
                else if (!filesSeenWhole)
                    filesSeenWhole = findFilesThatAreSeenWhole(program);
                if (isAtHead || filesSeenWhole->quickGet(index))
                    stored.quickSet(value.variable().index());
            }
            if (body.at(block).terminator.kind != TerminatorKind::Jump)
                break;
            block = body.at(block).terminator.next().block;
        }
    }
    stored.exclude(readAheadOfStore);
    if (stored.isEmpty())
        return stored;

    // An import is linked, and what it names may be read, before the file that it names has begun.
    BitVector modulesImported(program.modules.size());
    for (const Module& module : program.modules) {
        for (const ModuleRequest& request : module.requests) {
            if (request.kind != ModuleRequestKind::Require || request.isHoisted)
                modulesImported.quickSet(request.target.index());
        }
    }
    auto isAskedAbout = [&](FunctionID id, const Function& function, const Value& value) {
        if (!reads.contains(value.opcode) || !stored.quickGet(value.variable().index()))
            return false;
        ModuleID owner = program.at(value.variable()).module;
        return owner != function.module || (program.at(owner).body != id && !storedAtHead.quickGet(value.variable().index()));
    };
    // Whoever holds the module object of a file reads `exports` off it at any time.
    constexpr uint32_t isModuleWithObjectInHand = 1u << 31;
    Vector<uint32_t> readElsewhere = collectInParallel<uint32_t>(build.parallelLoops(), program.functions.size(), [&](unsigned index, Vector<uint32_t>& found) {
        const Function& function = program.functions[index];
        forEachListedValue(program, FunctionID(index), { Opcode::ModuleBuiltin }, [&](BlockID, uint32_t, ValueID, const Value& value) {
            if (program.builtins().canonicalID(value.builtin()) == Builtins::ModuleObject)
                found.append(value.module().index() | isModuleWithObjectInHand);
        });
        bool isAsked = false;
        forEachListedValue(program, FunctionID(index), reads, [&](BlockID, uint32_t, ValueID, const Value& value) {
            isAsked = isAsked || isAskedAbout(FunctionID(index), function, value);
        });
        if (!isAsked)
            return;
        for (const Block& block : function.blocks) {
            ModuleID begun;
            for (ValueID id : block.values) {
                const Value& value = function.at(id);
                if (value.opcode == Opcode::EvaluateModule)
                    begun = value.module();
                else if (isAskedAbout(FunctionID(index), function, value)) {
                    if (program.at(value.variable()).module != begun)
                        found.append(value.variable().index());
                } else if (!reads.contains(value.opcode) && !handsOperandOnUntested(value.opcode))
                    begun = { };
            }
        }
    });
    BitVector modulesWithObjectInHand(program.modules.size());
    for (uint32_t entry : readElsewhere) {
        if (entry & isModuleWithObjectInHand)
            modulesWithObjectInHand.quickSet(entry & ~isModuleWithObjectInHand);
        else
            stored.quickClear(entry);
    }
    for (size_t variable : BitVector(stored)) {
        uint32_t module = program.variables[variable].module.index();
        if (modulesImported.quickGet(module) || (modulesWithObjectInHand.quickGet(module) && !storedAtHead.quickGet(variable)))
            stored.quickClear(variable);
    }
    return stored;
}

UnobservedFieldDefinitions::UnobservedFieldDefinitions(const Program& program, BuildContext& build, NOESCAPE const ScopedLambda<bool(FunctionID)>& mayHoldDirectCall)
{
    m_variablesStoredAheadOfEveryRead = findVariablesStoredAheadOfEveryRead(program, build);
    Vector<InitializerCall> calls = collectInParallel<InitializerCall>(build.parallelLoops(), program.functions.size(), [&](unsigned functionIndex, Vector<InitializerCall>& found) {
        if (!mayHoldDirectCall(FunctionID(functionIndex)))
            return;
        const Function& function = program.functions[functionIndex];
        for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
            const BlockValues& values = function.blocks[blockIndex].values;
            for (uint32_t index = 0; index < values.size(); ++index) {
                const Value& value = function.at(values[index]);
                if (value.opcode == Opcode::CallDirect && program.at(value.function()).kind == FunctionKind::Initializer)
                    found.append(analyzeCall(program, build, FunctionID(functionIndex), BlockID(blockIndex), index));
            }
        }
    });
    AOT::stableSort(calls, { }, &InitializerCall::initializer);

    for (size_t first = 0, end = 0; first < calls.size(); first = end) {
        while (end < calls.size() && calls[end].initializer == calls[first].initializer)
            ++end;
        std::span<const InitializerCall> callsOfInitializer = calls.span().subspan(first, end - first);
        FunctionID initializer = calls[first].initializer;
        LayoutID classLayout = calls[first].classLayout;
        bool everyCallIsFollowed = std::ranges::all_of(callsOfInitializer, [&](const InitializerCall& call) {
            return call.observedKeys && call.classLayout == classLayout;
        });
        if (!everyCallIsFollowed || !build.isEnabled(FlowResultKind::Initialization, classLayout.index()))
            continue;

        for (const DefinitionOfUndefined& definition : summarizeInitializer(program, program.at(initializer)).definitionsOfUndefined) {
            bool isObserved = std::ranges::any_of(callsOfInitializer, [&](const InitializerCall& call) {
                return call.observedKeys->contains(definition.key);
            });
            if (isObserved || !program.at(classLayout).findField(definition.key))
                continue;
            bool isObservedWhereObjectRefusesDefinition = std::ranges::any_of(callsOfInitializer, [&](const InitializerCall& call) {
                return call.keysObservedWhereObjectRefusesDefinition.contains(definition.key);
            });
            m_definitions.append({ initializer, definition.definition, classLayout, definition.key, isObservedWhereObjectRefusesDefinition });
        }
    }
    AOT::sort(m_definitions, { }, [](const UnobservedFieldDefinition& definition) {
        return std::pair { definition.initializer, definition.definition };
    });
}

const UnobservedFieldDefinition* UnobservedFieldDefinitions::find(FunctionID initializer, ValueID definition) const
{
    auto found = std::ranges::lower_bound(m_definitions, std::pair { initializer, definition }, { }, [](const UnobservedFieldDefinition& entry) {
        return std::pair { entry.initializer, entry.definition };
    });
    return found != m_definitions.end() && found->initializer == initializer && found->definition == definition ? &*found : nullptr;
}

Vector<PropertyKeyID> keysDefinedRightAfterAllocation(const Program& program, FunctionID id, ValueID allocation)
{
    const Function& constructor = program.at(id);
    if (constructor.kind != FunctionKind::BaseConstructor)
        return { };
    auto isObject = [&](ValueID operand) {
        return operand && sameObjectThroughCasts(constructor, operand) == allocation;
    };
    const Value* firstUser = nullptr;
    for (const Block& block : constructor.blocks) {
        size_t position = block.values.find(allocation);
        if (position == notFound)
            continue;
        for (size_t index = position + 1; index < block.values.size() && !firstUser; ++index) {
            const Value& value = constructor.at(block.values[index]);
            if (!handsOperandOnUntested(value.opcode) && countOperands(value, isObject))
                firstUser = &value;
        }
        break;
    }
    if (!firstUser || firstUser->opcode != Opcode::CallDirect || program.at(firstUser->function()).kind != FunctionKind::Initializer)
        return { };
    const Function& initializer = program.at(firstUser->function());
    InitializerSummary summary = summarizeInitializer(program, initializer);
    if (!summary.usesObjectOnlyInDefinitions || countOperands(*firstUser, isObject) != 1)
        return { };
    unsigned thisIndex = *initializer.thisParameterIndex();
    if (thisIndex >= firstUser->argumentsOperands().size() || !isObject(firstUser->argumentsOperands()[thisIndex]))
        return { };
    Vector<PropertyKeyID> keys(summary.definedKeys.span());
    keys.removeAllMatching([&](PropertyKeyID key) {
        return program.at(key).kind == PropertyKeyKind::PrivateName;
    });
    return keys;
}

KeysOfLiterals keysDefinedBehindAllocationsOfLiterals(const Function& function)
{
    KeysOfLiterals keys;
    for (const Block& block : function.blocks) {
        for (ValueID id : block.values) {
            const Value& value = function.at(id);
            if (value.opcode == Opcode::FinishObject && function.at(value.operand(OperandIndex::FinishObject::object)).opcode == Opcode::NewObject)
                keys.add(value.operand(OperandIndex::FinishObject::object).index(), Vector<PropertyKeyID, 8>());
        }
    }
    if (keys.isEmpty())
        return keys;
    for (const Block& block : function.blocks) {
        for (ValueID id : block.values) {
            const Value& value = function.at(id);
            if (value.opcode != Opcode::PropDefine)
                continue;
            if (auto literal = keys.find(value.operand(OperandIndex::PropDefine::object).index()); literal != keys.end())
                literal->value.append(value.key());
        }
    }
    return keys;
}

KeysOfLiterals keysStoredRightAfterAllocations(const Program& program, const Function& function)
{
    KeysOfLiterals keys;
    BitVector isFresh;
    Vector<ValueID, 8> objectsOfBlock;
    unsigned numberOfFreshObjects = 0;
    // `module.exports = f, module.exports.default = f`: the one object that lies in a variable of the module, and what has read it back. Whoever else reads the variable runs later: nothing in between runs code.
    ValueID objectInVariable;
    VariableID variableOfObject;
    Vector<ValueID, 4> readsOfObject;
    for (const Block& block : function.blocks) {
        if (block.handler)
            continue;
        for (ValueID object : objectsOfBlock)
            isFresh.quickClear(object.index());
        objectsOfBlock.shrink(0);
        numberOfFreshObjects = 0;
        objectInVariable = { };
        auto forget = [&](ValueID object) {
            isFresh.quickClear(object.index());
            --numberOfFreshObjects;
            if (object == objectInVariable)
                objectInVariable = { };
        };
        for (ValueID id : block.values) {
            const Value& value = function.at(id);
            if (handsOperandOnUntested(value.opcode) || value.opcode == Opcode::TypeTest)
                continue;
            if (objectInVariable && value.opcode == Opcode::GetModuleVariable && value.variable() == variableOfObject) {
                readsOfObject.append(id);
                continue;
            }
            auto objectBehind = [&](ValueID operand) {
                ValueID object = operand ? sameObjectThroughCasts(function, operand).valueInFrontOfFacts() : ValueID();
                return object && objectInVariable && readsOfObject.contains(object) ? objectInVariable : object;
            };
            if (value.opcode == Opcode::PutModuleVariable && numberOfFreshObjects) {
                ValueID stored = objectBehind(value.valueOperand());
                if (objectInVariable && (stored != objectInVariable || value.variable() == variableOfObject))
                    forget(objectInVariable);
                if (stored && isFresh.get(stored.index()) && !objectInVariable && function.at(stored).opcode == Opcode::NewClosure) {
                    objectInVariable = stored;
                    variableOfObject = value.variable();
                    readsOfObject.shrink(0);
                    continue;
                }
            }

            bool isStoreByName = value.opcode == Opcode::PropSet && program.nameClassOf(value.key()) != NameClass::Proto;
            bool isStrictStore = isStoreByName && (value.mode() == ECMAMode::Strict || (objectInVariable && objectBehind(value.operand(*receiverOf(value.opcode))) == objectInVariable));
            if (objectInVariable && !isStrictStore && !isRematerializable(value) && value.opcode != Opcode::NewClosure)
                forget(objectInVariable);
            ValueID receiver;
            for (uint32_t index = 0; index < value.numberOfOperands() && numberOfFreshObjects; ++index) {
                ValueID object = objectBehind(value.operand(index));
                if (!object || !isFresh.get(object.index()))
                    continue;
                if ((isStrictStore || value.opcode == Opcode::PropDefine) && index == *receiverOf(value.opcode))
                    receiver = object;
                else if (object != objectInVariable)
                    forget(object);
            }
            if (isStrictStore && receiver && isFresh.get(receiver.index()) && program.at(value.key()).kind == PropertyKeyKind::Text && program.nameClassOf(value.key()) == NameClass::Ordinary)
                keys.add(receiver.index(), Vector<PropertyKeyID, 8>()).iterator->value.append(value.key());

            if (value.opcode == Opcode::NewObject ? !program.at(value.layout()).isEnvironment() : value.opcode == Opcode::NewClosure || (value.opcode == Opcode::Data && program.at(value.data()).root.is<DataNode::Object>())) {
                // A bit vector grows to the bit that is set and no further: thousands of objects would copy it thousands of times.
                isFresh.ensureSize(function.values.size());
                isFresh.quickSet(id.index());
                objectsOfBlock.append(id);
                ++numberOfFreshObjects;
            }
        }
    }
    return keys;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
