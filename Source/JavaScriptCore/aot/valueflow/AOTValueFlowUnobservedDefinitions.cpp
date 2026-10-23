/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowUnobservedDefinitions.h"

#if ENABLE(AOT)

#include "AOTAnalysisContext.h"
#include "AOTBuildContext.h"
#include "AOTFunctionAnalyses.h"
#include "AOTSchemaOperandIndexes.h"
#include "AOTSort.h"
#include "AOTSortedSet.h"
#include "AOTThreads.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include <algorithm>
#include <optional>
#include <utility>

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
    // Defined as undefined in front of an operation of the initializer that may run other code or throw.
    Keys keysShownToOtherCode;
    bool usesObjectOnlyInDefinitions { true };
    bool holdsItself { false };
};

struct InitializerCall {
    FunctionID initializer;
    LayoutID classLayout;
    bool observesEverything { true };
    Keys observedKeys;
};

struct ConstructionState {
    Keys storedKeys;
    bool holdsItself { false };
};

} // anonymous namespace

static bool isNeverEntered(const Function& function, BlockID block)
{
    return std::ranges::any_of(function.at(block).parameters, [&](ValueID parameter) {
        return function.at(parameter).type == TypeTable::never;
    });
}

static constexpr AbstractHeapSet propertyHeaps {
    AbstractHeap::ObjectFields, AbstractHeap::NamedProperties, AbstractHeap::ShapeWords, AbstractHeap::OrderWords, AbstractHeap::Elements, AbstractHeap::ArrayStorage
};

// An out-of-memory error is not observable behavior (see ThrowEffect).
static bool mayShowObjectToOtherCode(const Effects& effects)
{
    return effects.mayThrowByRule() || effects.mayBeTerminated || effects.mayRunOtherCode() || effects.reads.containsAny(propertyHeaps);
}

static bool handsOperandOnUntested(Opcode opcode)
{
    return isCastOrConversion(opcode) && !mayThrow(opcode);
}

static uint32_t countOperands(const Instruction& instruction, const Invocable<bool(ValueID)> auto& matches)
{
    uint32_t count = 0;
    for (uint32_t index = 0; index < instruction.numberOfOperands(); ++index)
        count += matches(instruction.operand(index));
    return count;
}

// A private name is the key of the field that its class declares.
static std::optional<PropertyKeyID> keyOfFieldAccess(const Program& program, const Value& value)
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

static InitializerSummary summarizeInitializer(const Program& program, const Function& initializer)
{
    InitializerSummary summary;
    summary.usesObjectOnlyInDefinitions = false;

    std::optional<unsigned> thisIndex = initializer.thisParameterIndex();
    if (!initializer.hasBody() || !thisIndex)
        return summary;
    CFG cfg(initializer);
    if (cfg.hasHandlers() || cfg.hasCycle())
        return summary;

    ValueIdentities identities(initializer, cfg);
    ValueID object = identities.identityOf(initializer.at(entryBlock).parameters[*thisIndex]);
    auto isObject = [&](ValueID id) {
        return id && identities.identityOf(id) == object;
    };
    Vector<ValueID, 4> closuresCapturingObject;
    auto aliasesObject = [&](ValueID id) {
        return id && (isObject(id) || closuresCapturingObject.contains(identities.identityOf(id)));
    };

    auto showDefinitions = [&] {
        for (const DefinitionOfUndefined& definition : summary.definitionsOfUndefined)
            summary.keysShownToOtherCode.add(definition.key);
    };

    Keys keysDefinedTwice;
    for (BlockID block : cfg.reversePostOrder()) {
        if (isNeverEntered(initializer, block))
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
            std::optional<PropertyKeyID> key = keyOfFieldAccess(program, value);
            if (!key) {
                // ToPropertyKey on an object runs user code.
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

namespace {

class ConstructionWalk {
public:
    ConstructionWalk(const Program& program, FunctionAnalyses& analyses, const InitializerSummary& summary, ValueID object, bool isKnownToOtherCode)
        : m_program(program)
        , m_analyses(analyses)
        , m_function(analyses.function())
        , m_summary(summary)
        , m_object(object)
        , m_isKnownToOtherCode(isKnownToOtherCode)
        , m_statesAtHead(m_function.blocks.size())
    {
    }

    std::optional<Keys> findObservedKeys(BlockID firstBlock, uint32_t firstIndex)
    {
        m_firstBlock = firstBlock;
        ConstructionState initialState { { }, m_summary.holdsItself };
        passOn(firstBlock, walk(firstBlock, firstIndex, initialState));
        while (std::exchange(m_hasChanged, false)) {
            forEachBlockBehindCall([&](BlockID block, const ConstructionState& state) {
                passOn(block, walk(block, 0, state));
            });
        }
        if (m_entersFirstBlockAgain)
            return std::nullopt;

        m_notesObservedKeys = true;
        walk(firstBlock, firstIndex, initialState);
        forEachBlockBehindCall([&](BlockID block, const ConstructionState& state) {
            walk(block, 0, state);
        });
        return WTF::move(m_observedKeys);
    }

private:
    bool isObject(ValueID id) { return id && m_analyses.identityOf(id) == m_object; }

    void forEachBlockBehindCall(NOESCAPE const Invocable<void(BlockID, const ConstructionState&)> auto& functor)
    {
        for (BlockID block : m_analyses.cfg().reversePostOrder()) {
            if (block == m_firstBlock || !m_statesAtHead[block.index()])
                continue;
            ConstructionState state = *m_statesAtHead[block.index()];
            functor(block, state);
        }
    }

    void meet(BlockID block, const ConstructionState& arriving)
    {
        std::optional<ConstructionState>& atHead = m_statesAtHead[block.index()];
        if (!atHead) {
            atHead = arriving;
            m_hasChanged = true;
            return;
        }
        size_t numberOfStoredKeys = atHead->storedKeys.size();
        bool heldItself = atHead->holdsItself;
        atHead->storedKeys.intersect(arriving.storedKeys);
        atHead->holdsItself |= arriving.holdsItself;
        m_hasChanged |= atHead->storedKeys.size() != numberOfStoredKeys || atHead->holdsItself != heldItself;
    }

    void passOn(BlockID block, const ConstructionState& state)
    {
        for (const Target& target : m_function.at(block).terminator.targets) {
            if (isNeverEntered(m_function, target.block))
                continue;
            if (target.block == m_firstBlock) {
                m_entersFirstBlockAgain = true;
                continue;
            }
            meet(target.block, state);
        }
    }

    void observe(const ConstructionState& state, PropertyKeyID key)
    {
        if (m_notesObservedKeys && !state.storedKeys.contains(key))
            m_observedKeys.add(key);
    }

    void observeEverything(const ConstructionState& state)
    {
        for (const DefinitionOfUndefined& definition : m_summary.definitionsOfUndefined)
            observe(state, definition.key);
    }

    std::optional<PropertyKeyID> keyOfDefinedProperty(const Value& value)
    {
        std::optional<PropertyKeyID> key = keyOfFieldAccess(m_program, value);
        if (!key || !isObject(value.objectOperand()) || !m_summary.definedKeys.contains(*key))
            return std::nullopt;
        return key;
    }

    void visit(ValueID id, ConstructionState& state)
    {
        const Value& value = m_function.at(id);
        if (handsOperandOnUntested(value.opcode) || value.opcode == Opcode::TypeTest)
            return;

        uint32_t usesOfObject = countOperands(value, [&](ValueID operand) {
            return isObject(operand);
        });
        if (!usesOfObject) {
            if (m_isKnownToOtherCode && mayShowObjectToOtherCode(m_analyses.effectsOf(id).onAnyEdge()))
                observeEverything(state);
            return;
        }
        std::optional<PropertyKeyID> key = keyOfDefinedProperty(value);
        if (!key) {
            observeEverything(state);
            return;
        }
        if (accessOf(value.opcode) == Access::Read) {
            if (state.holdsItself)
                observeEverything(state);
            else
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
            return m_isKnownToOtherCode;
        default:
            return false;
        }
    }

    bool mergesObjectWithOtherValues(const Terminator& terminator)
    {
        for (const Target& target : terminator.targets) {
            if (isNeverEntered(m_function, target.block))
                continue;
            const auto& parameters = m_function.at(target.block).parameters;
            for (uint32_t index = 0; index < target.arguments.size(); ++index) {
                if (isObject(target.arguments[index]) && !isObject(parameters[index]))
                    return true;
            }
        }
        return false;
    }

    ConstructionState walk(BlockID block, uint32_t firstIndex, ConstructionState state)
    {
        const BlockValues& values = m_function.at(block).values;
        for (uint32_t index = firstIndex; index < values.size(); ++index)
            visit(values[index], state);

        const Terminator& terminator = m_function.at(block).terminator;
        bool usesObject = countOperands(terminator, [&](ValueID operand) {
            return isObject(operand);
        });
        if (usesObject || leavesConstructor(terminator.kind) || mergesObjectWithOtherValues(terminator))
            observeEverything(state);
        return state;
    }

    const Program& m_program;
    FunctionAnalyses& m_analyses;
    const Function& m_function;
    const InitializerSummary& m_summary;
    ValueID m_object;
    bool m_isKnownToOtherCode;
    Vector<std::optional<ConstructionState>> m_statesAtHead;
    BlockID m_firstBlock;
    Keys m_observedKeys;
    bool m_hasChanged { false };
    bool m_entersFirstBlockAgain { false };
    bool m_notesObservedKeys { false };
};

} // anonymous namespace

static bool isFirstUseOfNewObject(FunctionAnalyses& analyses, BlockID block, uint32_t index, ValueID object)
{
    const Function& function = analyses.function();
    if (function.at(object).opcode != Opcode::NewObject)
        return false;
    const BlockValues& values = function.at(block).values;
    size_t allocation = values.find(object);
    if (allocation == notFound || allocation > index)
        return false;
    for (size_t earlier = allocation + 1; earlier < index; ++earlier) {
        const Value& value = function.at(values[earlier]);
        bool usesObject = countOperands(value, [&](ValueID operand) {
            return operand && analyses.identityOf(operand) == object;
        });
        if (usesObject && !handsOperandOnUntested(value.opcode))
            return false;
    }
    return true;
}

// `return other` in a constructor replaces the object under construction.
static bool returnsOnlyConstructedObject(const Program& program, BuildContext& build, FunctionID id)
{
    const Function& constructor = program.at(id);
    if (!constructor.hasBody() || (constructor.kind != FunctionKind::BaseConstructor && constructor.kind != FunctionKind::DerivedConstructor && constructor.kind != FunctionKind::Plain))
        return false;

    FunctionAnalyses analyses(AnalysisContext { build, program, nullptr, nullptr, id }, constructor);
    std::optional<unsigned> thisIndex = constructor.thisParameterIndex();
    ValueID thisParameter = thisIndex ? analyses.identityOf(constructor.at(entryBlock).parameters[*thisIndex]) : ValueID();
    Vector<ValueID, 8> worklist;
    for (BlockID block : analyses.cfg().reversePostOrder()) {
        if (isNeverEntered(constructor, block))
            continue;
        const Terminator& terminator = constructor.at(block).terminator;
        if (terminator.kind == TerminatorKind::ReturnCall)
            return false;
        if (terminator.kind == TerminatorKind::Return && terminator.valueOperand())
            worklist.append(terminator.valueOperand());
    }

    BitVector isVisited(constructor.values.size());
    while (!worklist.isEmpty()) {
        ValueID returned = analyses.identityOf(worklist.takeLast());
        if (isVisited.set(returned.index()) || returned == thisParameter || !program.typeOf(constructor, returned).hasObjects())
            continue;
        const Value& value = constructor.at(returned);
        if (value.opcode == Opcode::SuperConstruct || value.opcode == Opcode::SuperConstructList || (value.opcode == Opcode::NewObject && program.at(value.layout()).kind == LayoutKind::Instance))
            continue;
        if (value.opcode != Opcode::Parameter)
            return false;
        bool isMerge = false;
        for (BlockID predecessor : analyses.cfg().reversePostOrder()) {
            if (isNeverEntered(constructor, predecessor))
                continue;
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

// Whether super() in the constructor of the class hands back an ordinary object that was allocated for this construction, as long as the class keeps its declared parent.
static bool superReturnsNewObject(const Program& program, BuildContext& build, LayoutID classLayout)
{
    LayoutID current = classLayout;
    for (size_t steps = 0; steps < program.layouts.size() && program.at(current).has(LayoutFlags::HasHeritage); ++steps) {
        current = predictedHeritageOf(program.at(current));
        if (!current || !program.at(current).constructor || !returnsOnlyConstructedObject(program, build, program.at(current).constructor))
            return false;
    }
    return !program.at(current).has(LayoutFlags::HasHeritage);
}

static bool isDeclaredByBaseClass(const Program& program, LayoutID classLayout, PropertyKeyID key)
{
    LayoutID current = predictedHeritageOf(program.at(classLayout));
    for (size_t steps = 0; current && steps < program.layouts.size(); ++steps) {
        if (program.at(current).findField(key))
            return true;
        current = predictedHeritageOf(program.at(current));
    }
    return false;
}

static InitializerCall analyzeCall(const Program& program, BuildContext& build, FunctionID callerID, BlockID block, uint32_t index)
{
    const Function& caller = program.at(callerID);
    const Value& call = caller.at(caller.at(block).values[index]);
    const Function& initializer = program.at(call.function());
    InitializerCall result { call.function(), caller.constructedClass, true, { } };

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
    ValueID object = analyses.identityOf(call.argumentsOperands()[thisIndex]);
    bool isKnownToOtherCode = !isFirstUseOfNewObject(analyses, block, index, object);
    if (isKnownToOtherCode && !superReturnsNewObject(program, build, caller.constructedClass))
        return result;

    std::optional<Keys> observedKeys = ConstructionWalk(program, analyses, summary, object, isKnownToOtherCode).findObservedKeys(block, index + 1);
    if (!observedKeys)
        return result;
    result.observesEverything = false;
    result.observedKeys = WTF::move(*observedKeys);
    if (isKnownToOtherCode) {
        for (PropertyKeyID key : summary.keysShownToOtherCode.span())
            result.observedKeys.add(key);
    }
    // Redefining a property that a base class created can succeed on an object that the base class made non-extensible, where the next definition throws.
    for (const DefinitionOfUndefined& definition : summary.definitionsOfUndefined) {
        if (isDeclaredByBaseClass(program, caller.constructedClass, definition.key))
            result.observedKeys.add(definition.key);
    }
    return result;
}

static void collectCallsOfInitializers(const Program& program, BuildContext& build, FunctionID id, Vector<InitializerCall>& calls)
{
    const Function& function = program.at(id);
    for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        const BlockValues& values = function.blocks[blockIndex].values;
        for (uint32_t index = 0; index < values.size(); ++index) {
            const Value& value = function.at(values[index]);
            if (value.opcode == Opcode::CallDirect && program.at(value.function()).kind == FunctionKind::Initializer)
                calls.append(analyzeCall(program, build, id, BlockID(blockIndex), index));
        }
    }
}

UnobservedFieldDefinitions::UnobservedFieldDefinitions(const Program& program, BuildContext& build, NOESCAPE const ScopedLambda<bool(FunctionID)>& mayHoldDirectCall)
{
    Vector<InitializerCall> calls = collectInParallel<InitializerCall>(build.parallelLoops(), program.functions.size(), [&](unsigned index, Vector<InitializerCall>& found) {
        if (mayHoldDirectCall(FunctionID(index)))
            collectCallsOfInitializers(program, build, FunctionID(index), found);
    });
    AOT::stableSort(calls, { }, &InitializerCall::initializer);

    for (size_t first = 0; first < calls.size();) {
        size_t end = first + 1;
        while (end < calls.size() && calls[end].initializer == calls[first].initializer)
            ++end;
        std::span<const InitializerCall> callsOfInitializer = calls.span().subspan(first, end - first);
        first = end;

        FunctionID initializer = callsOfInitializer[0].initializer;
        LayoutID classLayout = callsOfInitializer[0].classLayout;
        bool everyCallIsFollowed = std::ranges::all_of(callsOfInitializer, [&](const InitializerCall& call) {
            return !call.observesEverything && call.classLayout == classLayout;
        });
        if (!everyCallIsFollowed || !build.isEnabled(FlowResultKind::Initialization, classLayout.index()))
            continue;

        for (const DefinitionOfUndefined& definition : summarizeInitializer(program, program.at(initializer)).definitionsOfUndefined) {
            bool isObserved = std::ranges::any_of(callsOfInitializer, [&](const InitializerCall& call) {
                return call.observedKeys.contains(definition.key);
            });
            if (!isObserved && program.at(classLayout).findField(definition.key))
                m_definitions.append({ initializer, definition.definition, classLayout, definition.key });
        }
    }
    AOT::sort(m_definitions, { }, [](const UnobservedFieldDefinition& definition) {
        return std::pair { definition.initializer, definition.definition };
    });
}

static Vector<PropertyKeyID> keysDefinedByCall(const Program& program, const Value& call, const Invocable<bool(ValueID)> auto& isObject)
{
    if (call.opcode != Opcode::CallDirect || program.at(call.function()).kind != FunctionKind::Initializer)
        return { };
    const Function& initializer = program.at(call.function());
    InitializerSummary summary = summarizeInitializer(program, initializer);
    if (!summary.usesObjectOnlyInDefinitions || countOperands(call, isObject) != 1)
        return { };
    unsigned thisIndex = *initializer.thisParameterIndex();
    if (thisIndex >= call.argumentsOperands().size() || !isObject(call.argumentsOperands()[thisIndex]))
        return { };
    Vector<PropertyKeyID> keys(summary.definedKeys.span());
    keys.removeAllMatching([&](PropertyKeyID key) {
        return program.at(key).kind == PropertyKeyKind::PrivateName;
    });
    return keys;
}

Vector<PropertyKeyID> keysDefinedRightAfterAllocation(const Program& program, FunctionID id, ValueID allocation)
{
    const Function& constructor = program.at(id);
    if (constructor.kind != FunctionKind::BaseConstructor)
        return { };
    auto isObject = [&](ValueID operand) {
        return operand && sameObjectThroughCasts(constructor, operand) == allocation;
    };

    for (const Block& block : constructor.blocks) {
        size_t position = block.values.find(allocation);
        if (position == notFound)
            continue;
        for (size_t index = position + 1; index < block.values.size(); ++index) {
            const Value& value = constructor.at(block.values[index]);
            if (!handsOperandOnUntested(value.opcode) && countOperands(value, isObject))
                return keysDefinedByCall(program, value, isObject);
        }
        return { };
    }
    return { };
}

KeysOfLiterals keysDefinedBehindAllocationsOfLiterals(const Function& function)
{
    KeysOfLiterals keys;
    for (const Block& block : function.blocks) {
        for (ValueID id : block.values) {
            const Value& value = function.at(id);
            if (value.opcode != Opcode::FinishObject)
                continue;
            ValueID literal = value.operand(OperandIndex::FinishObject::object);
            if (function.at(literal).opcode == Opcode::NewObject)
                keys.add(literal.index(), Vector<PropertyKeyID, 8>());
        }
    }
    if (keys.isEmpty())
        return keys;
    for (const Block& block : function.blocks) {
        for (ValueID id : block.values) {
            const Value& value = function.at(id);
            if (value.opcode != Opcode::PropDefine)
                continue;
            auto literal = keys.find(value.operand(OperandIndex::PropDefine::object).index());
            if (literal != keys.end())
                literal->value.append(value.key());
        }
    }
    return keys;
}

KeysOfLiterals keysStoredRightAfterAllocations(const Program& program, const Function& function)
{
    KeysOfLiterals keys;
    Vector<ValueID, 8> freshObjects;
    for (const Block& block : function.blocks) {
        if (block.handler)
            continue;
        freshObjects.shrink(0);
        for (ValueID id : block.values) {
            const Value& value = function.at(id);
            if (handsOperandOnUntested(value.opcode) || value.opcode == Opcode::TypeTest)
                continue;

            bool isStrictStore = value.opcode == Opcode::PropSet && value.mode() == ECMAMode::Strict && program.nameClassOf(value.key()) != NameClass::Proto;
            ValueID receiver;
            for (uint32_t index = 0; index < value.numberOfOperands() && !freshObjects.isEmpty(); ++index) {
                ValueID object = value.operand(index) ? sameObjectThroughCasts(function, value.operand(index)) : ValueID();
                if (!object || !freshObjects.contains(object))
                    continue;
                if ((isStrictStore || value.opcode == Opcode::PropDefine) && index == *receiverOf(value.opcode))
                    receiver = object;
                else
                    freshObjects.removeFirst(object);
            }
            if (isStrictStore && receiver && freshObjects.contains(receiver) && program.at(value.key()).kind == PropertyKeyKind::Text && program.nameClassOf(value.key()) == NameClass::Ordinary)
                keys.add(receiver.index(), Vector<PropertyKeyID, 8>()).iterator->value.append(value.key());

            if (value.opcode == Opcode::NewObject ? !program.at(value.layout()).isEnvironment() : value.opcode == Opcode::Data && program.at(value.data()).root.is<DataNode::Object>())
                freshObjects.append(id);
        }
    }
    return keys;
}

bool UnobservedFieldDefinitions::contains(FunctionID initializer, ValueID definition) const
{
    return std::ranges::binary_search(m_definitions, std::pair { initializer, definition }, { }, [](const UnobservedFieldDefinition& entry) {
        return std::pair { entry.initializer, entry.definition };
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
