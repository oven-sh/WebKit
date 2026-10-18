/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedPasses.h"

#if ENABLE(AOT)

#include "AOTConvention.h"
#include "AOTTypedThreads.h"
#include "FPRInfo.h"
#include <wtf/BitVector.h>
#include <wtf/Scope.h>

namespace JSC { namespace AOT { namespace Typed {

FunctionID functionPinnedToField(const Program& program, const Type& receiver, StringID name)
{
    if (program.mayRunInMultipleInstances)
        return { };
    auto access = fieldOf(program, receiver, name);
    if (!access)
        return { };
    FunctionID callee = program.at(access->layout).fields[access->field].singleFunction;
    if (!callee)
        return { };
    auto& target = program.at(callee);
    if (target.blocks.isEmpty() || target.isGenericEntrypoint() || target.rampForClosures || target.takesCalleeClosure || target.has(Format::FunctionHasRestParameter) || target.kind == FunctionKind::Constructor || target.kind == FunctionKind::Module)
        return { };
    if (program.at(access->layout).acceptsUntypedObjects || program.at(access->layout).subtypes.isEmpty())
        return { };
    constexpr uint32_t alwaysSet = fieldIsSetAtCreation | fieldIsNeverObservedAbsent;
    for (LayoutID subtype : program.at(access->layout).subtypes) {
        auto& layout = program.at(subtype);
        auto field = layout.fieldNamed(name);
        if (!field || layout.fields[*field].singleFunction != callee || (layout.fields[*field].flags & alwaysSet) != alwaysSet || layout.acceptsUntypedObjects || layout.mayDespecialize)
            return { };
        if (layout.kind == LayoutKind::Instance ? !layout.hasFixedLayout : layout.kind != LayoutKind::Record)
            return { };
    }
    return callee;
}

bool keepsFunctionPinned(const Program& program, const Function& function, const Field& field, ValueID object, ValueID stored)
{
    auto withoutCasts = [&](ValueID id) {
        while (function.at(id).opcode == Opcode::Cast)
            id = function.at(id).child(0);
        return id;
    };
    auto& closure = function.at(withoutCasts(stored));
    if (closure.opcode != Opcode::NewClosure && closure.opcode != Opcode::NewClosureOnce && closure.opcode != Opcode::FunctionReference)
        return false;
    if (closure.function() != field.singleFunction)
        return false;
    return !program.at(field.singleFunction).closuresCaptureOnlyTheirHolder || (closure.arguments.size() == 1 && withoutCasts(closure.arguments[0]) == withoutCasts(object));
}

bool replacesPinnedFunction(const Program& program, const Function& function, LayoutID created, const Value& store)
{
    auto& layout = program.at(created);
    if (!layout.hasFieldPinnedToFunction)
        return false;
    auto field = layout.fieldNamed(store.opcode == Opcode::PutField ? program.at(store.layout()).fields[store.field()].name : store.string());
    return field && layout.fields[*field].isPinnedToFunction && !keepsFunctionPinned(program, function, layout.fields[*field], store.child(0), store.child(1));
}

namespace {

static bool isAsyncFunctionRamp(const Function& function)
{
    return function.startFunction || (function.resumeFunction && (function.declaredAs & (functionIsAsync | functionIsGenerator)) == functionIsAsync);
}

const Function* asyncRampOf(const Program& program, const Function& callee)
{
    if (isAsyncFunctionRamp(callee))
        return &callee;
    if (callee.kind != FunctionKind::Bound || callee.blocks.isEmpty() || callee.blocks[0].values.isEmpty())
        return nullptr;
    auto& call = callee.at(callee.blocks[0].values.last());
    return call.opcode == Opcode::CallDirect && isAsyncFunctionRamp(program.at(call.function())) ? &program.at(call.function()) : nullptr;
}

TypeID promiseTypeOf(const Program& program, const Function& callee)
{
    auto* ramp = asyncRampOf(program, callee);
    if (!ramp)
        return { };
    for (auto& value : ramp->values) {
        if (value.opcode == Opcode::Operation && program.operationOf(value) == Operations::NewPromise)
            return value.type;
    }
    return { };
}

class CallResolution {
public:
    CallResolution(const Program& program, Function& function, bool mayGuess = true)
        : m_program(program)
        , m_function(function)
        , m_mayGuess(mayGuess)
    {
    }

    void collectBoundFunction(ValueID id, Vector<uint64_t>& wanted) const
    {
        auto& value = m_function.at(id);
        if (auto known = boundCalleeOf(value))
            wanted.append(Program::boundFunctionKey(known->function, boundArgumentCount(value)));
    }

    void expandDisposableStacks()
    {
        bool didLookUp = false;
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            for (unsigned i = 0; i < m_function.blocks[b].values.size(); ++i) {
                ValueID id = m_function.blocks[b].values[i];
                if (m_function.at(id).opcode != Opcode::Operation || m_program.operationOf(m_function.at(id)) != Operations::NewDisposableStack)
                    continue;
                if (!std::exchange(didLookUp, true)) {
                    m_usingNeedsObjectMessage = m_program.find(asBytes("The value of a using declaration must be an object, null or undefined"_s.span8())).value_or(StringID());
                    m_usingNeedsDisposeMessage = m_program.find(asBytes("The value of a using declaration has no callable dispose method"_s.span8())).value_or(StringID());
                    m_symbol = m_program.find(asBytes("Symbol"_s.span8())).value_or(StringID());
                    m_dispose = m_program.find(asBytes("dispose"_s.span8())).value_or(StringID());
                    m_typeError = m_program.find(asBytes("TypeError"_s.span8())).value_or(StringID());
                    m_suppressedError = m_program.find(asBytes("SuppressedError"_s.span8())).value_or(StringID());
                    countUsesAndFindUnreachableBlocks();
                }
                if (!m_usingNeedsObjectMessage || !m_usingNeedsDisposeMessage || !m_symbol || !m_dispose || !m_typeError || !m_suppressedError)
                    return;
                expandDisposableStack(BlockID(b), id);
            }
        }
    }

    unsigned run(CleanUpTiming timing)
    {
        recordRefutedCallees();
        unsigned resolved = 0;
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            for (unsigned i = 0; i < m_function.blocks[b].values.size(); ++i) {
                if (resolve(BlockID(b), i)) {
                    ++resolved;
                    break;
                }
            }
        }
        if (resolved && timing == CleanUpTiming::Immediate)
            cleanUp(m_program, m_function);
        return resolved;
    }

private:
    static constexpr unsigned maxCallees = 4;
    static constexpr uint32_t nonInlinedDepth = 1 << 16;

    struct Case {
        FunctionID callee;
        Vector<LayoutID, 4> subtypes;
        LayoutID test;
        bool isExact { false };
    };
    using Cases = Vector<Case, maxCallees>;

    struct KnownCallee {
        FunctionID function;
        ValueID definition;
        Arguments captures;
        bool isCertain { true };
        bool isCertainUnlessNullish { false };
        FunctionID other { };
        bool isClassFieldInitializer { false };
    };

    using Refuted = std::pair<ValueID, FunctionID>;
    using RefutedShape = std::pair<ValueID, LayoutID>;
    using PinnedFunction = std::tuple<LayoutID, StringID, bool>;

    enum class Yields : uint8_t {
        Result,
        Undefined,
    };

    const Type& typeOf(ValueID value) const { return m_program.typeOf(m_function, value); }
    bool isSubsetOf(ValueID value, TypeID type) const { return !type || typeOf(value).isSubsetOf(m_program.at(type), m_program); }

    ValueID originOf(ValueID value) const
    {
        while (m_function.at(value).opcode == Opcode::Cast)
            value = m_function.at(value).child(0);
        return value;
    }

    bool resolve(BlockID block, unsigned index)
    {
        switch (m_function.at(m_function.at(block).values[index]).opcode) {
        case Opcode::CallMethod:
            return resolvePinnedFieldCall(block, index) || resolveByShape(block, index, MethodKind::Method) || resolveOnClassObject(block, index) || resolvePredictedBuiltinCall(block, index) || resolveCallOrApply(block, index) || resolveBind(block, index) || resolveFieldCall(block, index);
        case Opcode::GetById:
            return resolveByShape(block, index, MethodKind::Getter) || resolveOnClassObject(block, index);
        case Opcode::PutById:
            return resolveByShape(block, index, MethodKind::Setter) || resolveOnClassObject(block, index);
        case Opcode::Call:
            return resolvePredictedMethodCall(block, index) || resolveMethodReadCall(block, index) || resolveCall(block, index) || resolvePredictedBuiltinCall(block, index) || resolveCallOrApply(block, index);
        case Opcode::Operation:
            return resolveConstruct(block, index) || resolveSuper(block, index) || resolveCallNoInline(block, index);
        case Opcode::Construct:
            return resolveFunctionConstruct(block, index) || resolvePredictedBuiltinCall(block, index);
        case Opcode::ConstructSuper:
            return resolveConstructSuper(block, index);
        case Opcode::CallBuiltin:
            return resolveCallNoInline(block, index);
        default:
            return false;
        }
    }

    BlockID addBlock()
    {
        m_function.blocks.append(Block { });
        m_refuted.append(Refuted { });
        m_classObjectFallbackBlocks.append(LayoutID());
        m_refutedShapes.append(RefutedShape { });
        m_pinnedFunctionBlocks.append(PinnedFunction { });
        return BlockID(m_function.blocks.size() - 1);
    }

    Value make(Opcode opcode, TypeID type) const
    {
        Value value;
        value.opcode = opcode;
        value.position = m_position;
        value.inlineFrame = m_inlineFrame;
        value.isInTailPosition = m_isInTailPosition && (opcode == Opcode::Call || opcode == Opcode::CallMethod || opcode == Opcode::CallDirect || opcode == Opcode::CallBuiltin);
        value.type = type;
        value.rep = repOf(m_program.at(type));
        return value;
    }

    ValueID addValue(Value&& value)
    {
        m_function.values.append(WTF::move(value));
        return ValueID(m_function.values.size() - 1);
    }

    void open(BlockID block, unsigned index)
    {
        ValueID id = m_function.at(block).values[index];
        m_position = m_function.at(id).position;
        m_inlineFrame = m_function.at(id).inlineFrame;
        m_isInTailPosition = m_function.at(id).isInTailPosition;
        bool hasDepth = m_function.at(id).opcode == Opcode::CallMethod || m_function.at(id).opcode == Opcode::Call || m_function.at(id).opcode == Opcode::Operation;
        m_inliningDepth = hasDepth ? m_function.at(id).immediates[1] : Format::none;
        m_resultCast = std::nullopt;
        m_hasHandler = m_function.at(block).terminator.kind == TerminatorKind::Invoke && index + 1 == m_function.at(block).values.size();
        m_after = addBlock();
        {
            auto& from = m_function.at(block);
            m_function.at(m_after).values.append(from.values.subspan(index + 1));
            from.values.shrink(index);
            m_function.at(m_after).terminator = std::exchange(from.terminator, Terminator { });
        }
        m_handler = { };
        if (m_hasHandler) {
            auto& invoke = m_function.at(m_after).terminator;
            m_handler = invoke.targets[1];
            invoke.kind = TerminatorKind::Jump;
            invoke.targets.shrink(1);
        }
        Value parameter = make(Opcode::Parameter, m_function.at(id).type);
        parameter.copyPredictionFrom(m_function.at(id));
        parameter.immediates = { m_after.index(), 0 };
        m_function.at(id) = WTF::move(parameter);
        m_function.at(m_after).parameters.append(id);
        moveTo(block);
    }

    void moveTo(BlockID block)
    {
        m_block = block;
        m_lastMayThrow = false;
    }

    void endBlockAfterThrowingValue()
    {
        if (!m_lastMayThrow)
            return;
        m_lastMayThrow = false;
        BlockID next = addBlock();
        auto& invoke = m_function.at(m_block).terminator;
        invoke.kind = TerminatorKind::Invoke;
        invoke.targets.append(Target { next, { } });
        invoke.targets.append(m_handler);
        m_block = next;
    }

    void place(ValueID value)
    {
        endBlockAfterThrowingValue();
        m_function.at(m_block).values.append(value);
    }

    ValueID append(Value&& value)
    {
        ValueID id = addValue(WTF::move(value));
        place(id);
        return id;
    }

    ValueID appendThrowing(Value&& value)
    {
        ValueID id = append(WTF::move(value));
        m_lastMayThrow = m_hasHandler;
        return id;
    }

    BlockID branch(ValueID condition)
    {
        endBlockAfterThrowingValue();
        BlockID then = addBlock();
        BlockID otherwise = addBlock();
        auto& branch = m_function.at(m_block).terminator;
        branch.kind = TerminatorKind::Branch;
        branch.value = condition;
        branch.targets.append(Target { then, { } });
        branch.targets.append(Target { otherwise, { } });
        moveTo(then);
        return otherwise;
    }

    void finish(ValueID result)
    {
        auto& terminator = m_function.at(m_block).terminator;
        terminator.kind = m_lastMayThrow ? TerminatorKind::Invoke : TerminatorKind::Jump;
        terminator.targets.append(Target { m_after, Arguments { result } });
        if (m_lastMayThrow)
            terminator.targets.append(m_handler);
        m_lastMayThrow = false;
    }

    void finishThrowing()
    {
        if (!std::exchange(m_lastMayThrow, false)) {
            m_function.at(m_block).terminator.kind = TerminatorKind::Unreachable;
            return;
        }
        BlockID never = addBlock();
        auto& terminator = m_function.at(m_block).terminator;
        terminator.kind = TerminatorKind::Invoke;
        terminator.targets.append(Target { never, { } });
        terminator.targets.append(m_handler);
    }

    void jumpTo(BlockID block, Arguments&& arguments)
    {
        auto& terminator = m_function.at(m_block).terminator;
        terminator.kind = m_lastMayThrow ? TerminatorKind::Invoke : TerminatorKind::Jump;
        terminator.targets.append(Target { block, WTF::move(arguments) });
        if (m_lastMayThrow)
            terminator.targets.append(m_handler);
        m_lastMayThrow = false;
    }

    void finishByThrowing(ValueID thrown)
    {
        endBlockAfterThrowingValue();
        auto& terminator = m_function.at(m_block).terminator;
        terminator.kind = TerminatorKind::Throw;
        terminator.value = thrown;
        if (m_hasHandler)
            terminator.targets.append(m_handler);
    }

    ValueID appendGlobal(StringID name)
    {
        Value global = make(Opcode::GetGlobal, m_program.types.anything);
        global.immediates[0] = name.index();
        return appendThrowing(WTF::move(global));
    }

    ValueID appendGlobalConstruct(StringID name, Arguments&& arguments)
    {
        Value construct = make(Opcode::Construct, m_program.types.anything);
        construct.children[0] = appendGlobal(name);
        construct.arguments = WTF::move(arguments);
        return appendThrowing(WTF::move(construct));
    }

    void finishByThrowingTypeError(StringID message)
    {
        Value text = make(Opcode::String, m_program.types.string);
        text.immediates[0] = message.index();
        finishByThrowing(appendGlobalConstruct(m_typeError, Arguments { append(WTF::move(text)) }));
    }

    ValueID addParameterTo(BlockID block, TypeID type)
    {
        Value parameter = make(Opcode::Parameter, type);
        parameter.immediates = { block.index(), static_cast<uint32_t>(m_function.at(block).parameters.size()) };
        ValueID id = addValue(WTF::move(parameter));
        m_function.at(block).parameters.append(id);
        return id;
    }

    ValueID insertUndefinedAtBlockStart(BlockID block)
    {
        ValueID id = addValue(make(Opcode::Undefined, m_program.types.undefined));
        auto& values = m_function.at(block).values;
        values.insert(!values.isEmpty() && m_function.at(values[0]).opcode == Opcode::CatchValue, id);
        return id;
    }

    ValueID appendTest(BuiltinID test, ValueID operand)
    {
        Value value = make(Opcode::CallBuiltin, m_program.types.boolean);
        value.immediates[0] = test.index();
        value.arguments.append(operand);
        return append(WTF::move(value));
    }

    void countUsesAndFindUnreachableBlocks()
    {
        m_numberOfUses.fill(0, m_function.values.size());
        auto count = [&](ValueID child) {
            if (child)
                ++m_numberOfUses[child.index()];
        };
        for (auto& block : m_function.blocks) {
            block.terminator.forEachChild(count);
            for (ValueID id : block.values)
                m_function.at(id).forEachChild(count);
        }
        m_originalBlockCount = m_function.blocks.size();
        Vector<unsigned, 16> worklist { 0u };
        m_isReached.set(0);
        while (!worklist.isEmpty()) {
            for (auto& target : m_function.blocks[worklist.takeLast()].terminator.targets) {
                if (!m_isReached.set(target.block.index()))
                    worklist.append(target.block.index());
            }
        }
        for (unsigned b = 0; b < m_originalBlockCount; ++b) {
            if (m_isReached.get(b))
                continue;
            for (auto& target : m_function.blocks[b].terminator.targets)
                m_unreachablePredecessors.add(target.block.index(), Vector<unsigned, 1>()).iterator->value.append(b);
        }
    }

    bool expandDisposableStack(BlockID definedIn, ValueID stack)
    {
        static constexpr unsigned maxResources = 4;
        static constexpr uint8_t isNotAdded = 1;
        static constexpr uint8_t isAdded = 2;
        static constexpr uint8_t mayBeAdded = isNotAdded | isAdded;
        struct Resource {
            ValueID method;
            ValueID value;
            BlockID addedIn;
            Vector<uint8_t> atEntryOf;
            Vector<ValueID> parametersOf;
        };
        if (definedIn.index() < m_originalBlockCount && !m_isReached.get(definedIn.index()))
            return false;
        while (m_indexInRegion.size() < m_function.blocks.size())
            m_indexInRegion.append(UINT_MAX);
        Vector<unsigned, 32> region { definedIn.index() };
        m_indexInRegion[definedIn.index()] = 0;
        auto clearRegionOnExit = makeScopeExit([&] {
            for (unsigned block : region)
                m_indexInRegion[block] = UINT_MAX;
        });
        Vector<Resource, maxResources> resources;
        ValueID dispose;
        BlockID disposedIn;
        unsigned seenUseCount = 0;
        for (unsigned next = 0; next < region.size(); ++next) {
            unsigned b = region[next];
            auto& block = m_function.blocks[b];
            bool hasOtherUses = false;
            block.terminator.forEachChild([&](ValueID child) {
                hasOtherUses |= child == stack;
            });
            for (unsigned i = 0; i < block.values.size() && !hasOtherUses; ++i) {
                auto& value = m_function.at(block.values[i]);
                unsigned uses = 0;
                value.forEachChild([&](ValueID child) {
                    uses += child == stack;
                });
                if (!uses)
                    continue;
                seenUseCount += uses;
                if (uses > 1 || value.opcode != Opcode::Operation || value.arguments.size() != 3 || value.arguments[0] != stack) {
                    hasOtherUses = true;
                    continue;
                }
                if (m_program.operationOf(value) == Operations::DisposeResources && !dispose) {
                    dispose = block.values[i];
                    disposedIn = BlockID(b);
                    continue;
                }
                auto& isAsync = m_function.at(value.arguments[2]);
                bool isWatched = i + 1 == block.values.size() && block.terminator.kind == TerminatorKind::Invoke;
                if (m_program.operationOf(value) != Operations::AddDisposableResource || isAsync.opcode != Opcode::Boolean || isAsync.boolean() || !isWatched || resources.size() == maxResources) {
                    hasOtherUses = true;
                    continue;
                }
                resources.append(Resource { block.values[i], value.arguments[1], BlockID(b), { }, { } });
            }
            if (hasOtherUses)
                return false;
            if (dispose && disposedIn == BlockID(b))
                continue;
            bool exitsWithoutStack = BlockID(b) == definedIn && block.terminator.kind == TerminatorKind::Invoke && block.values.last() == stack;
            for (unsigned t = 0; t < block.terminator.targets.size() - exitsWithoutStack; ++t) {
                unsigned to = block.terminator.targets[t].block.index();
                if (m_indexInRegion[to] != UINT_MAX)
                    continue;
                m_indexInRegion[to] = 0;
                region.append(to);
            }
        }
        if (seenUseCount != m_numberOfUses[stack.index()] || !dispose || disposedIn == definedIn)
            return false;
        for (auto& resource : resources) {
            if (m_numberOfUses[resource.method.index()])
                return false;
        }
        std::ranges::sort(region);
        unsigned regionSize = region.size();
        for (unsigned i = 0; i < regionSize; ++i)
            m_indexInRegion[region[i]] = i;
        std::ranges::sort(resources, { }, [](const Resource& resource) {
            return resource.addedIn.index();
        });
        unsigned disposal = m_indexInRegion[disposedIn.index()];

        auto stateAtExitOf = [&](const Resource& resource, unsigned block, unsigned target) -> uint8_t {
            if (BlockID(block) == resource.addedIn && !target)
                return isAdded;
            if (BlockID(block) == definedIn)
                return isNotAdded;
            unsigned index = m_indexInRegion[block];
            return index == UINT_MAX ? 0 : resource.atEntryOf[index];
        };
        for (auto& resource : resources) {
            if (resource.addedIn == disposedIn)
                return false;
            resource.atEntryOf.fill(0, regionSize);
            Vector<unsigned, 16> worklist { definedIn.index() };
            while (!worklist.isEmpty()) {
                unsigned block = worklist.takeLast();
                if (BlockID(block) == disposedIn)
                    continue;
                auto& targets = m_function.blocks[block].terminator.targets;
                for (unsigned t = 0; t < targets.size(); ++t) {
                    unsigned to = targets[t].block.index();
                    if (m_indexInRegion[to] == UINT_MAX)
                        continue;
                    uint8_t& atEntry = resource.atEntryOf[m_indexInRegion[to]];
                    uint8_t joined = atEntry | stateAtExitOf(resource, block, t);
                    if (std::exchange(atEntry, joined) != joined)
                        worklist.append(to);
                }
            }
            if (!resource.atEntryOf[disposal])
                return false;
            if (resource.addedIn != definedIn && resource.atEntryOf[m_indexInRegion[resource.addedIn.index()]] != isNotAdded)
                return false;
        }
        Vector<unsigned, maxResources> inAdditionOrder;
        inAdditionOrder.fill(UINT_MAX, resources.size());
        for (unsigned later = 0; later < resources.size(); ++later) {
            unsigned addedBeforeCount = 0;
            for (unsigned earlier = 0; earlier < resources.size(); ++earlier) {
                if (earlier == later)
                    continue;
                uint8_t state = resources[later].addedIn == definedIn ? isNotAdded : resources[earlier].atEntryOf[m_indexInRegion[resources[later].addedIn.index()]];
                if (state == mayBeAdded)
                    return false;
                addedBeforeCount += state == isAdded;
            }
            if (inAdditionOrder[addedBeforeCount] != UINT_MAX)
                return false;
            inAdditionOrder[addedBeforeCount] = later;
        }

        BitVector reachesDisposal(regionSize);
        {
            Vector<Vector<unsigned, 2>> predecessors(regionSize);
            for (unsigned b : region) {
                for (auto& target : m_function.blocks[b].terminator.targets) {
                    if (unsigned to = m_indexInRegion[target.block.index()]; to != UINT_MAX)
                        predecessors[to].append(b);
                }
            }
            Vector<unsigned, 16> worklist { disposedIn.index() };
            reachesDisposal.set(disposal);
            while (!worklist.isEmpty()) {
                unsigned block = worklist.takeLast();
                if (BlockID(block) == definedIn)
                    continue;
                for (unsigned predecessor : predecessors[m_indexInRegion[block]]) {
                    if (!reachesDisposal.set(m_indexInRegion[predecessor]))
                        worklist.append(predecessor);
                }
            }
        }

        m_position = m_function.at(stack).position;
        m_inlineFrame = m_function.at(stack).inlineFrame;
        m_isInTailPosition = false;
        TypeID anything = m_program.types.anything;
        Vector<unsigned, 32> sources(region);
        for (auto& resource : resources) {
            resource.parametersOf.fill(ValueID(), 2 * regionSize);
            for (unsigned i = 0; i < regionSize; ++i) {
                unsigned b = region[i];
                if (BlockID(b) == definedIn || resource.atEntryOf[i] != mayBeAdded || !reachesDisposal.get(i))
                    continue;
                resource.parametersOf[2 * i] = addParameterTo(BlockID(b), anything);
                resource.parametersOf[2 * i + 1] = addParameterTo(BlockID(b), anything);
                if (auto unreachable = m_unreachablePredecessors.find(b); unreachable != m_unreachablePredecessors.end())
                    sources.appendVector(unreachable->value);
            }
        }
        if (sources.size() > regionSize) {
            std::ranges::sort(sources);
            sources.shrink(std::ranges::unique(sources).begin() - sources.begin());
        }
        for (unsigned b : sources) {
            ValueID nothing;
            unsigned from = m_indexInRegion[b];
            auto& targets = m_function.blocks[b].terminator.targets;
            for (unsigned t = 0; t < targets.size(); ++t) {
                unsigned to = m_indexInRegion[targets[t].block.index()];
                if (to == UINT_MAX)
                    continue;
                for (auto& resource : resources) {
                    if (!resource.parametersOf[2 * to])
                        continue;
                    uint8_t state = stateAtExitOf(resource, b, t);
                    RELEASE_ASSERT(state != mayBeAdded || resource.parametersOf[2 * from]);
                    if (state != isAdded && state != mayBeAdded && !nothing)
                        nothing = insertUndefinedAtBlockStart(BlockID(b));
                    targets[t].arguments.append(state == isAdded ? resource.value : state == mayBeAdded ? resource.parametersOf[2 * from] : nothing);
                    targets[t].arguments.append(state == isAdded ? resource.method : state == mayBeAdded ? resource.parametersOf[2 * from + 1] : nothing);
                }
            }
        }

        for (auto& resource : resources) {
            open(resource.addedIn, m_function.at(resource.addedIn).values.size() - 1);
            m_function.at(resource.method).type = anything;
            m_function.at(resource.method).rep = repOf(m_program.at(anything));
            BlockID isNotObject = branch(appendTest(Operations::IsObject, resource.value));
            Value symbol = make(Opcode::GetById, anything);
            symbol.children[0] = appendGlobal(m_symbol);
            symbol.immediates[0] = m_dispose.index();
            Value read = make(Opcode::GetByVal, anything);
            read.children = { resource.value, appendThrowing(WTF::move(symbol)) };
            ValueID method = appendThrowing(WTF::move(read));
            BlockID isNotCallable = branch(appendTest(Operations::IsCallable, method));
            finish(method);
            moveTo(isNotCallable);
            finishByThrowingTypeError(m_usingNeedsDisposeMessage);
            moveTo(isNotObject);
            Value isNil = make(Opcode::IsUndefinedOrNull, m_program.types.boolean);
            isNil.children[0] = resource.value;
            BlockID isNeither = branch(append(WTF::move(isNil)));
            finish(append(make(Opcode::Undefined, m_program.types.undefined)));
            moveTo(isNeither);
            finishByThrowingTypeError(m_usingNeedsObjectMessage);
        }

        ValueID hasError = m_function.at(dispose).arguments[1];
        ValueID error = m_function.at(dispose).arguments[2];
        open(disposedIn, m_function.at(disposedIn).values.find(dispose));
        Value truth = make(Opcode::Boolean, m_program.types.boolean);
        truth.immediates[0] = 1;
        ValueID yes = append(WTF::move(truth));
        Value falsehood = make(Opcode::Boolean, m_program.types.boolean);
        falsehood.immediates[0] = 0;
        ValueID hasNewError = append(WTF::move(falsehood));
        for (unsigned i = resources.size(); i--;) {
            auto& resource = resources[inAdditionOrder[i]];
            uint8_t state = resource.atEntryOf[disposal];
            if (state == isNotAdded)
                continue;
            ValueID value = state == isAdded ? resource.value : resource.parametersOf[2 * disposal];
            ValueID method = state == isAdded ? resource.method : resource.parametersOf[2 * disposal + 1];
            BlockID next = addBlock();
            ValueID hasErrorAfter = addParameterTo(next, m_program.types.boolean);
            ValueID errorAfter = addParameterTo(next, anything);
            ValueID hasNewErrorAfter = addParameterTo(next, m_program.types.boolean);
            Value isNone = make(Opcode::StrictEq, m_program.types.boolean);
            isNone.children = { method, append(make(Opcode::Undefined, m_program.types.undefined)) };
            BlockID isSome = branch(append(WTF::move(isNone)));
            jumpTo(next, Arguments { hasError, error, hasNewError });
            moveTo(isSome);
            BlockID caught = addBlock();
            Value call = make(Opcode::Call, anything);
            call.children = { method, value };
            call.immediates[1] = m_inliningDepth;
            m_function.at(m_block).values.append(addValue(WTF::move(call)));
            auto& invoke = m_function.at(m_block).terminator;
            invoke.kind = TerminatorKind::Invoke;
            invoke.targets.append(Target { next, Arguments { hasError, error, hasNewError } });
            invoke.targets.append(Target { caught, { } });
            moveTo(caught);
            ValueID thrown = append(make(Opcode::CatchValue, anything));
            BlockID isFirst = branch(hasError);
            ValueID suppressing = appendGlobalConstruct(m_suppressedError, Arguments { thrown, error });
            jumpTo(next, Arguments { yes, suppressing, yes });
            moveTo(isFirst);
            jumpTo(next, Arguments { yes, thrown, yes });
            moveTo(next);
            hasError = hasErrorAfter;
            error = errorAfter;
            hasNewError = hasNewErrorAfter;
        }
        BlockID hasNoNewError = branch(hasNewError);
        finishByThrowing(error);
        moveTo(hasNoNewError);
        finish(append(make(Opcode::Undefined, m_program.types.undefined)));
        m_function.at(stack) = make(Opcode::Undefined, m_program.types.undefined);
        return true;
    }

    void absorbResultCast(ValueID id)
    {
        auto& after = m_function.at(m_after);
        if (after.values.isEmpty() || (after.values.size() == 1 && after.terminator.kind == TerminatorKind::Invoke))
            return;
        ValueID cast = after.values[0];
        auto& value = m_function.at(cast);
        if (value.opcode != Opcode::Cast || value.child(0) != id || (value.castReason() != CastReason::As && value.castReason() != CastReason::Coerce))
            return;
        if (!isUsedOnlyBy(id, cast))
            return;
        m_resultCast = value;
        Value parameter = make(Opcode::Parameter, value.type);
        parameter.copyPredictionFrom(value);
        parameter.immediates = { m_after.index(), 0 };
        m_function.at(cast) = WTF::move(parameter);
        after.values.removeAt(0);
        after.parameters[0] = cast;
    }

    void finishWithCast(ValueID result)
    {
        if (m_resultCast) {
            Value cast = *m_resultCast;
            cast.children[0] = result;
            result = appendThrowing(WTF::move(cast));
        }
        finish(result);
    }

    ValueID held(ValueID value, TypeID type, CastReason reason)
    {
        if (!type || m_program.castIsRedundant(typeOf(value), m_program.at(type), reason))
            return value;
        Value cast = make(Opcode::Cast, type);
        cast.children[0] = value;
        cast.immediates[1] = static_cast<uint32_t>(reason);
        if (reason == CastReason::As && m_unsharedLiterals.contains(value))
            cast.immediates[1] |= static_cast<uint32_t>(Format::Freshness::TopLevel) << Format::freshnessShift;
        return appendThrowing(WTF::move(cast));
    }

    ValueID castToParameterType(FunctionID callee, ValueID value, TypeID type, CastReason reason)
    {
        ValueID result = held(value, type, reason);
        if (result != value && !m_program.at(callee).has(Format::FunctionIsBuiltin | Format::FunctionIsInBuiltinModule | Format::FunctionIsHiddenInStackTraces))
            m_function.at(result).setParameterCheckCallee(callee);
        return result;
    }

    bool definesFieldOf(const Value& user, ValueID literal) const
    {
        if ((user.opcode != Opcode::Operation && user.opcode != Opcode::CallBuiltin) || user.arguments.isEmpty() || user.arguments[0] != literal)
            return false;
        return m_program.operationOf(user) == Operations::DefineField;
    }

    Vector<ValueID, 2> literalsPassedToRecordParameters(ValueID call, FunctionID callee) const
    {
        Vector<ValueID, 2> result;
        auto& arguments = m_function.at(call).arguments;
        auto& parameters = m_program.at(callee).parameterTypes;
        for (unsigned k = 0; k < std::min<size_t>(arguments.size(), parameters.size()); ++k) {
            ValueID literal = arguments[k];
            if (m_function.at(literal).opcode != Opcode::NewObject || !parameters[k])
                continue;
            bool becomesRecord = m_program.at(parameters[k]).objectLayouts.containsIf([&](LayoutID supertype) {
                return m_program.at(supertype).kind == LayoutKind::Record && !m_program.at(supertype).acceptsUncheckedObjects();
            });
            if (!becomesRecord)
                continue;
            bool isSharedElsewhere = false;
            for (auto& block : m_function.blocks) {
                for (ValueID id : block.values) {
                    auto& user = m_function.at(id);
                    unsigned times = 0;
                    user.forEachChild([&](ValueID child) {
                        times += child == literal;
                    });
                    isSharedElsewhere |= times > 1 || (times && id != call && !definesFieldOf(user, literal));
                }
                block.terminator.forEachChild([&](ValueID child) {
                    isSharedElsewhere |= child == literal;
                });
            }
            if (!isSharedElsewhere)
                result.append(literal);
        }
        return result;
    }

    bool mayBeCalledDirectly(FunctionID id) const
    {
        auto& callee = m_program.at(id);
        return !callee.takesCalleeClosure && !callee.rampForClosures && mayBeCalledDirectlyWithClosure(id);
    }

    bool mayBeCalledDirectlyWithClosure(FunctionID id) const
    {
        auto& callee = m_program.at(id);
        if (callee.blocks.isEmpty() || callee.isGenericEntrypoint())
            return false;
        if (callee.kind == FunctionKind::Constructor || callee.kind == FunctionKind::Module)
            return false;
        return canPassRestArguments(callee);
    }

    bool canPassRestArguments(const Function& callee) const
    {
        if (!callee.has(Format::FunctionHasRestParameter))
            return true;
        if (callee.parameterTypes.isEmpty())
            return false;
        TypeID rest = callee.parameterTypes.last();
        return !rest || Type::engineClass(Builtins::singleton().arrayClass()).isSubsetOf(m_program.at(rest), m_program);
    }

    Arguments passedTo(FunctionID id, std::span<const ValueID> captures, ValueID thisValue, std::span<const ValueID> given)
    {
        auto& callee = m_program.at(id);
        Arguments all;
        RELEASE_ASSERT(captures.size() == callee.captureTypes.size());
        all.append(captures);
        if (callee.has(Format::FunctionHasThis))
            all.append(castToParameterType(id, thisValue, callee.thisType, CastReason::Flow));
        unsigned count = callee.parameterTypes.size();
        for (unsigned k = 0; k < count; ++k) {
            TypeID type = callee.parameterTypes[k];
            if (callee.has(Format::FunctionHasRestParameter) && k + 1 == count) {
                Value rest = make(Opcode::NewArray, type ? type : m_program.types.anything);
                if (k < given.size())
                    rest.arguments.append(given.subspan(k));
                all.append(appendThrowing(WTF::move(rest)));
            } else if (k < given.size())
                all.append(castToParameterType(id, given[k], type, callee.parameterCastReason(k)));
            else
                all.append(castToParameterType(id, append(make(Opcode::Undefined, m_program.types.undefined)), type, callee.parameterCastReason(k)));
        }
        return all;
    }

    ValueID call(FunctionID id, Arguments&& arguments)
    {
        TypeID result = m_program.at(id).resultType;
        Value call = make(Opcode::CallDirect, result ? result : m_program.types.anything);
        call.immediates = { id.index(), m_inliningDepth };
        call.arguments = WTF::move(arguments);
        return appendThrowing(WTF::move(call));
    }

    bool needsArgumentCasts(FunctionID id, ValueID thisValue, std::span<const ValueID> given) const
    {
        auto& callee = m_program.at(id);
        auto isHeld = [&](const Type& have, TypeID type, CastReason reason) {
            return !type || m_program.castIsRedundant(have, m_program.at(type), reason);
        };
        if (callee.has(Format::FunctionHasThis) && !isHeld(typeOf(thisValue), callee.thisType, CastReason::Flow))
            return true;
        for (unsigned k = 0; k < callee.parameterTypes.size(); ++k) {
            if (!isHeld(k < given.size() ? typeOf(given[k]) : m_program.at(m_program.types.undefined), callee.parameterTypes[k], callee.parameterCastReason(k)))
                return true;
        }
        return false;
    }

    bool mayTakeUntypedObjects(FunctionID id) const
    {
        for (TypeID type : m_program.at(id).parameterTypes) {
            if (!type)
                continue;
            for (LayoutID supertype : m_program.at(type).objectLayouts) {
                if (m_program.at(supertype).acceptsUntypedObjects)
                    return true;
            }
        }
        return false;
    }

    void callAndFinish(FunctionID id, std::span<const ValueID> captures, ValueID thisValue, std::span<const ValueID> given, TypeID type, Yields yields)
    {
        if (FunctionID written = m_program.at(id).declaredGenericEntrypoint; written && (needsArgumentCasts(id, thisValue, given) || (m_program.at(written).has(FunctionLogsTypeMismatches) && mayTakeUntypedObjects(id))))
            id = written;
        else if (FunctionID copy = m_program.at(id).selfCheckingCopy; copy && !m_isInTailPosition && !m_program.at(type).isAnything())
            id = copy;
        TypeID promiseType = promiseTypeOf(m_program, m_program.at(id));
        bool wasWatched = m_hasHandler;
        Target handler = m_handler;
        BlockID refused;
        BlockID before;
        if (promiseType) {
            endBlockAfterThrowingValue();
            before = m_block;
            refused = addBlock();
            m_hasHandler = true;
            m_handler = Target { refused, { } };
        }
        Arguments arguments = passedTo(id, captures, thisValue, given);
        bool mayRefuse = promiseType && (m_lastMayThrow || m_block != before);
        if (promiseType) {
            endBlockAfterThrowingValue();
            m_hasHandler = wasWatched;
            m_handler = handler;
        }
        ValueID result = call(id, WTF::move(arguments));
        if (yields == Yields::Undefined)
            result = append(make(Opcode::Undefined, m_program.types.undefined));
        finishWithCast(held(result, type, CastReason::Claim));
        if (!mayRefuse)
            return;
        moveTo(refused);
        ValueID error = append(make(Opcode::CatchValue, m_program.types.anything));
        Value newPromise = make(Opcode::Operation, promiseType);
        newPromise.immediates[0] = Operations::NewPromise.index();
        ValueID promise = append(WTF::move(newPromise));
        Value reject = make(Opcode::Operation, m_program.types.undefined);
        reject.immediates[0] = Operations::RejectPromise.index();
        reject.arguments = { promise, error };
        append(WTF::move(reject));
        finishWithCast(held(promise, type, CastReason::Claim));
    }

    bool isFirstOnPrototypeChain(LayoutID subtype, FunctionID callee) const
    {
        for (LayoutID at = subtype; at; at = m_program.at(at).base) {
            auto& layout = m_program.at(at);
            if (layout.hasComputedMethodName)
                return false;
            for (auto& method : layout.methods) {
                if (method.function == callee)
                    return true;
            }
        }
        return false;
    }

    bool hasNoInstances(LayoutID subtype) const
    {
        auto& layout = m_program.at(subtype);
        return layout.hasNoOwnInstances && !layout.mayHaveOtherPrototype;
    }

    std::optional<Cases> casesOf(LayoutID supertype, StringID name, MethodKind kind, bool isTestedForMembership = false) const
    {
        if (m_program.at(supertype).acceptsUntypedObjects && !isTestedForMembership)
            return std::nullopt;
        Cases cases;
        for (LayoutID subtype : m_program.at(supertype).subtypes) {
            if (hasNoInstances(subtype))
                continue;
            auto& layout = m_program.at(subtype);
            FunctionID callee;
            if (hasMethodField(m_program, subtype, name))
                return std::nullopt;
            if (layout.kind == LayoutKind::Instance && !layout.fieldNamed(name)) {
                callee = findMethod(m_program, subtype, name, kind);
                bool isOnTheInstance = isPrivateName(m_program.string(name));
                if (!callee || (layout.mayHaveOtherPrototype && !isOnTheInstance) || !mayBeCalledDirectly(callee) || !m_program.at(callee).captureTypes.isEmpty() || (!isOnTheInstance && !isFirstOnPrototypeChain(subtype, callee)))
                    return std::nullopt;
            } else if (kind != MethodKind::Method)
                return std::nullopt;
            size_t at = cases.findIf([&](auto& known) {
                return known.callee == callee;
            });
            if (at == notFound) {
                if (cases.size() == maxCallees)
                    return std::nullopt;
                at = cases.size();
                cases.append(Case { callee, { }, { }, false });
            }
            cases[at].subtypes.append(subtype);
        }
        if (cases.isEmpty())
            return std::nullopt;
        return cases;
    }

    bool decides(LayoutID supertype, const Case& which, const Vector<LayoutID>& left) const
    {
        auto& subtypes = m_program.at(supertype).subtypes;
        for (LayoutID subtype : left) {
            if (std::ranges::binary_search(subtypes, subtype) != std::ranges::binary_search(which.subtypes, subtype))
                return false;
        }
        return true;
    }

    bool findTest(Case& which, const Vector<LayoutID>& left) const
    {
        which.isExact = false;
        if (TypeID thisType = which.callee ? m_program.at(which.callee).thisType : TypeID()) {
            which.test = m_program.at(thisType).singleLayout();
            if (which.test && decides(which.test, which, left))
                return true;
        }
        which.test = which.subtypes[0];
        if (decides(which.test, which, left))
            return true;
        which.isExact = true;
        return which.subtypes.size() == 1;
    }

    bool orderCases(Cases& cases, LayoutID supertype) const
    {
        for (unsigned i = 0; i + 1 < cases.size(); ++i) {
            if (!cases[i].callee)
                std::rotate(cases.begin() + i, cases.begin() + i + 1, cases.end());
        }
        unsigned numberOfCallees = cases.size() - (cases.last().callee ? 0 : 1);
        Vector<LayoutID> left = m_program.at(supertype).subtypes;
        left.removeAllMatching([&](LayoutID subtype) {
            return hasNoInstances(subtype);
        });
        for (unsigned done = 0; done + 1 < cases.size(); ++done) {
            unsigned found = done;
            while (found < numberOfCallees && !findTest(cases[found], left))
                ++found;
            if (found == numberOfCallees)
                return false;
            std::rotate(cases.begin() + done, cases.begin() + found, cases.begin() + found + 1);
            left.removeAllMatching([&](LayoutID subtype) {
                return std::ranges::binary_search(cases[done].subtypes, subtype);
            });
        }
        return true;
    }

    LayoutID predictedLayoutOf(BlockID block, ValueID object) const
    {
        if (!m_mayGuess || !typeOf(object).anyObject)
            return { };
        TypeID prediction = m_function.at(object).prediction() ? m_function.at(object).prediction() : m_function.at(originOf(object)).prediction();
        if (!prediction)
            return { };
        LayoutID supertype = m_program.at(prediction).singleLayout();
        if (!supertype || m_refutedShapes[block.index()] == RefutedShape { originOf(object), supertype })
            return { };
        return supertype;
    }

    BlockID testShape(ValueID object, LayoutID supertype)
    {
        Value test = make(Opcode::HasShapeOf, m_program.types.boolean);
        test.children[0] = object;
        test.immediates = { supertype.index(), m_program.mayRunInMultipleInstances ? shapeTestIsForThisInstance : 0u };
        BlockID otherwise = branch(append(WTF::move(test)));
        m_refutedShapes[otherwise.index()] = RefutedShape { originOf(object), supertype };
        return otherwise;
    }

    bool resolvePredictedMethodCall(BlockID block, unsigned index)
    {
        if (!m_mayGuess)
            return false;
        ValueID id = m_function.at(block).values[index];
        ValueID read = m_function.at(id).child(0);
        if (m_function.at(read).opcode != Opcode::GetById)
            return false;
        ValueID object = m_function.at(read).child(0);
        if (typeOf(object).singleLayout() || originOf(object) != originOf(m_function.at(id).child(1)))
            return false;
        LayoutID supertype = predictedLayoutOf(block, object);
        if (!supertype || !isObjectLayout(m_program.at(supertype).kind) || !isUsedOnlyBy(read, id))
            return false;
        Value original = m_function.at(id);
        auto cases = casesOf(supertype, m_function.at(read).string(), MethodKind::Method);
        if (!cases || !orderCases(*cases, supertype) || !cases->last().callee)
            return false;
        unsigned readIndex = index;
        while (readIndex && m_function.at(block).values[readIndex - 1] != read) {
            Opcode between = m_function.at(m_function.at(block).values[--readIndex]).opcode;
            if (between != Opcode::Undefined && between != Opcode::Null && between != Opcode::Boolean && between != Opcode::Integer && between != Opcode::Double && between != Opcode::String)
                return false;
        }
        if (!readIndex)
            return false;
        m_function.at(block).values.removeAt(readIndex - 1);
        open(block, index - 1);
        BlockID otherwise = testShape(object, supertype);
        Value cast = make(Opcode::Cast, m_program.at(supertype).type);
        cast.children[0] = object;
        cast.immediates = { m_function.at(read).immediates[0], static_cast<uint32_t>(CastReason::Flow) };
        ValueID receiver = appendThrowing(WTF::move(cast));
        dispatch(*cases, supertype, receiver, original.arguments.span(), original, Yields::Result);
        moveTo(otherwise);
        m_function.at(read).setPredictedClosure({ });
        place(read);
        m_lastMayThrow = m_hasHandler;
        finish(appendThrowing(WTF::move(original)));
        return true;
    }

    bool resolveByShape(BlockID block, unsigned index, MethodKind kind)
    {
        Value original = m_function.at(m_function.at(block).values[index]);
        ValueID object = original.child(0);
        StringID name = original.string();
        LayoutID supertype = typeOf(object).singleLayout();
        bool isExpected = !supertype;
        if (isExpected)
            supertype = predictedLayoutOf(block, object);
        if (!supertype || !isObjectLayout(m_program.at(supertype).kind))
            return false;
        bool needsMembershipTest = !isExpected && (kind != MethodKind::Method || isPrivateName(m_program.string(name))) && m_program.at(supertype).acceptsUntypedObjects;
        auto cases = casesOf(supertype, name, kind, needsMembershipTest);
        if (!cases || !orderCases(*cases, supertype) || (isExpected && !cases->last().callee))
            return false;
        bool callsFunction = std::ranges::any_of(*cases, [](auto& which) { return !!which.callee; });
        bool needsInstanceTest = !isExpected && m_program.mayRunInMultipleInstances && callsFunction;
        if ((needsInstanceTest || needsMembershipTest) && m_refutedShapes[block.index()] == RefutedShape { originOf(object), supertype })
            return false;
        Arguments given;
        if (kind == MethodKind::Method)
            given = original.arguments;
        else if (kind == MethodKind::Setter)
            given.append(original.child(1));

        open(block, index);
        ValueID receiver = object;
        BlockID otherwise;
        if (isExpected || needsInstanceTest || needsMembershipTest)
            otherwise = testShape(object, supertype);
        if ((*cases)[0].callee && (isExpected || typeOf(object).mayBeUndefinedOrNull())) {
            Value cast = make(Opcode::Cast, m_program.at(supertype).type);
            cast.children[0] = object;
            cast.immediates = { name.index(), static_cast<uint32_t>(CastReason::Flow) };
            receiver = appendThrowing(WTF::move(cast));
        }
        dispatch(*cases, supertype, receiver, given.span(), original, kind == MethodKind::Setter ? Yields::Undefined : Yields::Result);
        if (otherwise) {
            moveTo(otherwise);
            ValueID result = appendThrowing(Value(original));
            if (kind == MethodKind::Setter)
                result = held(append(make(Opcode::Undefined, m_program.types.undefined)), original.type, CastReason::Claim);
            finish(result);
        }
        return true;
    }

    ValueID narrowedToThisType(const Case& which, ValueID receiver)
    {
        auto& callee = m_program.at(which.callee);
        LayoutID declaring = callee.thisType ? m_program.at(callee.thisType).singleLayout() : LayoutID();
        if (!callee.declaredGenericEntrypoint || !declaring)
            return receiver;
        auto& subtypes = m_program.at(declaring).subtypes;
        bool isProven = std::ranges::all_of(which.subtypes, [&](LayoutID subtype) {
            return std::ranges::binary_search(subtypes, subtype);
        });
        return isProven ? held(receiver, callee.thisType, CastReason::Flow) : receiver;
    }

    void dispatch(const Cases& cases, LayoutID supertype, ValueID receiver, std::span<const ValueID> given, const Value& original, Yields yields)
    {
        for (unsigned c = 0; c < cases.size(); ++c) {
            auto& which = cases[c];
            BlockID otherwise;
            if (c + 1 < cases.size()) {
                Value test = make(Opcode::HasShapeOf, m_program.types.boolean);
                test.children[0] = receiver;
                test.immediates = { which.test.index(), which.isExact ? 1u : 0u };
                otherwise = branch(append(WTF::move(test)));
            }
            if (which.callee)
                callAndFinish(which.callee, { }, narrowedToThisType(which, receiver), given, original.type, yields);
            else
                callFieldValueAndFinish(supertype, receiver, original);
            if (otherwise)
                moveTo(otherwise);
        }
    }

    bool resolveMethodReadCall(BlockID block, unsigned index)
    {
        ValueID id = m_function.at(block).values[index];
        Value original = m_function.at(id);
        ValueID read = original.child(0);
        if (m_function.at(read).opcode != Opcode::GetById)
            return false;
        ValueID object = m_function.at(read).child(0);
        StringID name = m_function.at(read).string();
        LayoutID supertype = typeOf(object).singleLayout();
        if (originOf(object) != originOf(original.child(1)) || !supertype || !isObjectLayout(m_program.at(supertype).kind) || m_program.mayRunInMultipleInstances)
            return false;
        auto cases = casesOf(supertype, name, MethodKind::Method);
        if (!cases || !orderCases(*cases, supertype) || !cases->last().callee || !isUsedOnlyBy(read, id))
            return false;

        bool mayBeNullish = typeOf(object).mayBeUndefinedOrNull();
        auto& receiver = m_function.at(read);
        receiver.opcode = Opcode::Cast;
        receiver.type = mayBeNullish ? m_program.at(supertype).type : m_function.at(object).type;
        receiver.rep = repOf(m_program.at(receiver.type));
        receiver.immediates = { mayBeNullish ? name.index() : Format::none, static_cast<uint32_t>(mayBeNullish ? CastReason::Flow : CastReason::Upcast) };
        if (!mayBeNullish) {
            for (auto& other : m_function.blocks) {
                if (other.terminator.kind != TerminatorKind::Invoke || other.values.isEmpty() || other.values.last() != read)
                    continue;
                other.terminator.kind = TerminatorKind::Jump;
                other.terminator.targets.shrink(1);
            }
        }
        open(block, index);
        dispatch(*cases, supertype, read, original.arguments.span(), original, Yields::Result);
        return true;
    }

    void callFieldValueAndFinish(LayoutID supertype, ValueID receiver, const Value& original)
    {
        TypeID type;
        if (auto field = m_program.at(supertype).fieldNamed(original.string()))
            type = m_program.at(supertype).fields[*field].type;
        Value read = make(Opcode::GetById, type ? type : m_program.types.anything);
        read.children[0] = receiver;
        read.immediates[0] = original.immediates[0];
        ValueID callee = appendThrowing(WTF::move(read));
        Value call = make(Opcode::Call, original.type);
        call.children = { callee, receiver };
        call.immediates = { original.immediates[0], m_inliningDepth };
        call.setCallSignature(original.callSignature());
        call.arguments = original.arguments;
        finish(appendThrowing(WTF::move(call)));
    }

    bool resolvePinnedFieldCall(BlockID block, unsigned index)
    {
        Value original = m_function.at(m_function.at(block).values[index]);
        ValueID object = original.child(0);
        LayoutID supertype = typeOf(object).singleLayout();
        if (!supertype || m_pinnedFunctionBlocks[block.index()] != PinnedFunction { supertype, original.string(), true })
            return false;
        FunctionID callee = functionPinnedToField(m_program, typeOf(object), original.string());
        if (!callee)
            return false;
        open(block, index);
        ValueID receiver = object;
        if (typeOf(object).mayBeUndefinedOrNull()) {
            Value cast = make(Opcode::Cast, m_program.at(supertype).type);
            cast.children[0] = object;
            cast.immediates = { original.string().index(), static_cast<uint32_t>(CastReason::Flow) };
            receiver = appendThrowing(WTF::move(cast));
        }
        Arguments captures;
        auto access = fieldOf(m_program, typeOf(object), original.string());
        bool capturesReceiver = m_program.at(callee).closuresCaptureOnlyTheirHolder
            && m_program.castIsRedundant(typeOf(receiver), m_program.at(m_program.at(callee).captureTypes[0]), CastReason::Flow)
            && std::ranges::all_of(m_program.at(access->layout).subtypes, [&](LayoutID subtype) {
                return m_program.at(subtype).kind == LayoutKind::Instance;
            });
        if (capturesReceiver)
            captures.append(receiver);
        else if (!m_program.at(callee).captureTypes.isEmpty()) {
            Value read = make(Opcode::GetById, m_program.at(access->layout).fields[access->field].type);
            read.children[0] = receiver;
            read.immediates[0] = original.immediates[0];
            ValueID closure = appendThrowing(WTF::move(read));
            captures = capturesOf(KnownCallee { callee, closure, { }, true }, closure);
        }
        callAndFinish(callee, captures.span(), receiver, original.arguments.span(), original.type, Yields::Result);
        return true;
    }

    bool resolveFieldCall(BlockID block, unsigned index)
    {
        Value original = m_function.at(m_function.at(block).values[index]);
        auto access = fieldOf(m_program, typeOf(original.child(0)), original.string());
        if (!access || !m_program.at(access->layout).fields[access->field].singleFunction)
            return false;
        open(block, index);
        callFieldValueAndFinish(access->layout, original.child(0), original);
        return true;
    }

    struct Member {
        FunctionID method;
        FunctionID getter;
        FunctionID setter;
    };

    LayoutID classOfClassObject(ValueID value) const
    {
        auto& definition = m_function.at(originOf(value));
        return definition.opcode == Opcode::FunctionReference ? m_program.at(definition.function()).constructedClass : LayoutID();
    }

    std::optional<Member> lookupMember(LayoutID start, StringID name, bool isStatic) const
    {
        for (LayoutID at = start; at; at = m_program.at(at).base) {
            auto& layout = m_program.at(at);
            if (isStatic ? !layout.hasImmutableMembers || layout.hasComputedStaticMemberName : !layout.hasFixedLayout || layout.hasComputedMethodName)
                return std::nullopt;
            bool isStaticField = isStatic && layout.staticFields.containsIf([&](auto& field) {
                return field.name == name;
            });
            if (isStaticField)
                return std::nullopt;
            Member member;
            for (auto& method : isStatic ? layout.staticMethods : layout.methods) {
                if (method.name != name)
                    continue;
                if (method.mayBeReplaced)
                    return std::nullopt;
                (method.kind == MethodKind::Method ? member.method : method.kind == MethodKind::Getter ? member.getter : member.setter) = method.function;
            }
            if (member.method)
                return member.getter || member.setter ? std::nullopt : std::optional(member);
            if (member.getter || member.setter)
                return member;
            auto text = m_program.string(name);
            if (isStatic && (equalSpans(text, asBytes("length"_s.span8())) || equalSpans(text, asBytes("name"_s.span8())) || equalSpans(text, asBytes("prototype"_s.span8()))))
                return std::nullopt;
        }
        return std::nullopt;
    }

    bool mayBeCalledWithoutEnvironment(FunctionID id) const { return mayBeCalledDirectly(id) && m_program.at(id).captureTypes.isEmpty(); }

    void guardClassObjectUnmodified(LayoutID ofClass, const Value& original)
    {
        if (!ofClass)
            return;
        Value test = make(Opcode::IsClassObjectUnmodified, m_program.types.boolean);
        test.immediates[0] = ofClass.index();
        BlockID asks = branch(append(WTF::move(test)));
        BlockID isUnmodified = m_block;
        m_classObjectFallbackBlocks[asks.index()] = ofClass;
        moveTo(asks);
        Value fallback(original);
        if (original.opcode == Opcode::ConstructSuper || original.opcode == Opcode::ConstructSuperForwardVarargs)
            fallback.isOnSlowPath = true;
        ValueID result = appendThrowing(WTF::move(fallback));
        if (original.opcode == Opcode::PutById)
            result = held(append(make(Opcode::Undefined, m_program.types.undefined)), original.type, CastReason::Claim);
        finish(result);
        moveTo(isUnmodified);
    }

    bool readMember(BlockID block, unsigned index, const Member& member, ValueID thisValue, TypeID type, LayoutID guarded = { })
    {
        Value original = m_function.at(m_function.at(block).values[index]);
        if (member.getter) {
            if (!mayBeCalledWithoutEnvironment(member.getter))
                return false;
            open(block, index);
            guardClassObjectUnmodified(guarded, original);
            callAndFinish(member.getter, { }, thisValue, { }, type, Yields::Result);
            return true;
        }
        if (member.method && (!m_program.at(member.method).captureTypes.isEmpty() || !m_program.genericEntrypoints.contains(member.method.index())))
            return false;
        open(block, index);
        guardClassObjectUnmodified(guarded, original);
        Value read = member.method ? make(Opcode::FunctionReference, m_program.types.function) : make(Opcode::Undefined, m_program.types.undefined);
        if (member.method)
            read.immediates[0] = member.method.index();
        finish(held(append(WTF::move(read)), type, CastReason::Claim));
        return true;
    }

    bool storeToMember(BlockID block, unsigned index, const Member& member, ValueID thisValue, ValueID stored, TypeID type, LayoutID guarded = { })
    {
        if (!member.setter || !mayBeCalledWithoutEnvironment(member.setter))
            return false;
        Value original = m_function.at(m_function.at(block).values[index]);
        open(block, index);
        guardClassObjectUnmodified(guarded, original);
        callAndFinish(member.setter, { }, thisValue, std::span { &stored, 1 }, type, Yields::Undefined);
        return true;
    }

    bool resolveStaticMethodReadCall(BlockID block, unsigned index, FunctionID method, ValueID classObject, LayoutID ofClass)
    {
        if (!mayBeCalledWithoutEnvironment(method))
            return false;
        auto& values = m_function.at(block).values;
        ValueID read = values[index];
        unsigned callIndex = index + 1;
        for (; callIndex < values.size(); ++callIndex) {
            Opcode between = m_function.at(values[callIndex]).opcode;
            if (between != Opcode::Undefined && between != Opcode::Null && between != Opcode::Boolean && between != Opcode::Integer && between != Opcode::Double && between != Opcode::String)
                break;
        }
        if (callIndex == values.size())
            return false;
        ValueID id = values[callIndex];
        Value original = m_function.at(id);
        if (original.opcode != Opcode::Call || original.child(0) != read || originOf(original.child(1)) != originOf(classObject) || !isUsedOnlyBy(read, id))
            return false;
        values.removeAt(index);
        open(block, callIndex - 1);
        Value test = make(Opcode::IsClassObjectUnmodified, m_program.types.boolean);
        test.immediates[0] = ofClass.index();
        BlockID asks = branch(append(WTF::move(test)));
        BlockID isUnmodified = m_block;
        m_classObjectFallbackBlocks[asks.index()] = ofClass;
        moveTo(asks);
        Value call = original;
        if (m_function.at(classObject).opcode == Opcode::FunctionReference && original.child(1) == classObject) {
            ValueID again = append(Value(m_function.at(classObject)));
            m_function.at(read).children[0] = again;
            call.children[1] = again;
        }
        place(read);
        m_lastMayThrow = m_hasHandler;
        finish(appendThrowing(WTF::move(call)));
        moveTo(isUnmodified);
        callAndFinish(method, { }, classObject, original.arguments.span(), original.type, Yields::Result);
        return true;
    }

    bool accessStaticField(BlockID block, unsigned index, LayoutID ofClass)
    {
        Value& access = m_function.at(m_function.at(block).values[index]);
        auto& fields = m_program.at(ofClass).staticFields;
        size_t field = fields.findIf([&](auto& field) {
            return field.name == access.string();
        });
        if (field == notFound || fields[field].place == Format::none)
            return false;
        bool isStore = access.opcode == Opcode::PutById;
        if (isStore && !m_program.castIsRedundant(typeOf(access.child(1)), m_program.at(fields[field].type), CastReason::Claim))
            return false;
        if (Value& receiver = m_function.at(access.child(0)); receiver.opcode == Opcode::FunctionReference && isUsedOnlyBy(access.child(0), m_function.at(block).values[index])) {
            receiver.opcode = Opcode::Undefined;
            receiver.type = m_program.types.undefined;
            receiver.rep = repOf(m_program.at(receiver.type));
            receiver.immediates = { Format::none, Format::none };
        }
        access.children = { isStore ? access.child(1) : ValueID(), ValueID(), ValueID() };
        access.opcode = isStore ? Opcode::PutStaticField : Opcode::GetStaticField;
        access.immediates = { ofClass.index(), static_cast<uint32_t>(field) };
        return true;
    }

    bool resolveOnClassObject(BlockID block, unsigned index)
    {
        Value original = m_function.at(m_function.at(block).values[index]);
        ValueID classObject = original.child(0);
        LayoutID ofClass = classOfClassObject(classObject);
        if (!ofClass || m_classObjectFallbackBlocks[block.index()] == ofClass)
            return false;
        if ((original.opcode == Opcode::GetById || original.opcode == Opcode::PutById) && accessStaticField(block, index, ofClass))
            return false;
        auto member = lookupMember(ofClass, original.string(), true);
        if (!member)
            return false;
        switch (original.opcode) {
        case Opcode::GetById:
            if (member->method && resolveStaticMethodReadCall(block, index, member->method, classObject, ofClass))
                return true;
            return readMember(block, index, *member, classObject, original.type, ofClass);
        case Opcode::PutById:
            return storeToMember(block, index, *member, classObject, original.child(1), original.type, ofClass);
        default:
            break;
        }
        if (!member->method || !mayBeCalledWithoutEnvironment(member->method))
            return false;
        open(block, index);
        guardClassObjectUnmodified(ofClass, original);
        callAndFinish(member->method, { }, classObject, original.arguments.span(), original.type, Yields::Result);
        return true;
    }

    bool resolveSuper(BlockID block, unsigned index)
    {
        Value original = m_function.at(m_function.at(block).values[index]);
        bool isGet = m_program.operationOf(original) == Operations::SuperGet;
        if ((!isGet && m_program.operationOf(original) != Operations::SuperSet) || original.arguments.size() != (isGet ? 4u : 5u))
            return false;
        LayoutID around = classOfClassObject(original.arguments[0]);
        ValueID keyID = m_program.constantOf(m_function, original.arguments[1]);
        ValueID isStaticID = m_program.constantOf(m_function, original.arguments.last());
        if (!keyID || !isStaticID)
            return false;
        auto& key = m_function.at(keyID);
        auto& isStatic = m_function.at(isStaticID);
        if (!around || key.opcode != Opcode::String || isStatic.opcode != Opcode::Boolean || (isStatic.boolean() && m_classObjectFallbackBlocks[block.index()] == m_program.at(around).base))
            return false;
        if (!m_program.at(around).hasFixedLayout || !m_program.at(around).hasImmutableMembers)
            return false;
        auto member = lookupMember(m_program.at(around).base, key.string(), isStatic.boolean());
        if (!member)
            return false;
        LayoutID guarded = isStatic.boolean() ? m_program.at(around).base : LayoutID();
        if (isGet)
            return readMember(block, index, *member, original.arguments[2], original.type, guarded);
        return storeToMember(block, index, *member, original.arguments[3], original.arguments[2], original.type, guarded);
    }

    std::optional<KnownCallee> knownCallee(ValueID value) const
    {
        ValueID operand = value;
        FunctionID predicted;
        bool isClassFieldInitializer = false;
        value = originOf(value);
        auto& definition = m_function.at(value);
        switch (definition.opcode) {
        case Opcode::FunctionReference:
            return KnownCallee { definition.function(), value, { }, true };
        case Opcode::NewClosure:
        case Opcode::NewClosureOnce:
            return KnownCallee { definition.function(), value, definition.arguments, true };
        case Opcode::GetModuleVariable: {
            auto& variable = m_program.variables[definition.variable().index()];
            if (variable.singleFunctionIsProven || variable.singleFunctionOrNullish)
                return KnownCallee { variable.singleFunction, value, { }, variable.singleFunctionIsProven, true };
            predicted = variable.singleFunction;
            break;
        }
        case Opcode::GetCapture: {
            auto& holder = m_function.at(originOf(definition.child(0)));
            if (holder.opcode != Opcode::GetModuleVariable)
                break;
            auto& variable = m_program.variables[holder.variable().index()];
            auto& captured = variable.functionsCapturedBySingleFunction;
            uint32_t index = definition.immediates[1];
            if (variable.singleFunction == definition.function() && (variable.singleFunctionIsProven || variable.singleFunctionOrNullish) && index < captured.size() && captured[index])
                return KnownCallee { captured[index], value, { }, true };
            break;
        }
        case Opcode::GetField: {
            auto& field = m_program.at(definition.layout()).fields[definition.field()];
            if (field.singleFunctionIsProven || field.singleFunctionOrNullish)
                return KnownCallee { field.singleFunction, value, { }, field.singleFunctionIsProven, true };
            if (field.otherFunction)
                return KnownCallee { field.singleFunction, value, { }, false, false, field.otherFunction };
            predicted = field.singleFunction;
            break;
        }
        case Opcode::GetById:
            if (auto access = fieldOf(m_program, typeOf(definition.child(0)), definition.string())) {
                auto& layout = m_program.at(access->layout);
                auto& field = layout.fields[access->field];
                predicted = field.singleFunction;
                constexpr uint32_t alwaysSet = fieldIsSetAtCreation | fieldIsNeverObservedAbsent;
                isClassFieldInitializer = predicted && layout.kind == LayoutKind::Instance && layout.hasFixedLayout && (field.flags & alwaysSet) == alwaysSet;
            } else if (definition.predictedByKey())
                predicted = m_program.functionByKey(definition.predictedByKey(), definition.string());
            break;
        case Opcode::GetByVal:
            if (ValueID key = m_program.constantOf(m_function, definition.child(1)); definition.predictedByKey() && key && m_function.at(key).opcode == Opcode::String)
                predicted = m_program.functionByKey(definition.predictedByKey(), m_function.at(key).string());
            break;
        default:
            break;
        }
        if (FunctionID ofValue = m_function.at(operand).predictedClosure() ? m_function.at(operand).predictedClosure() : definition.predictedClosure()) {
            isClassFieldInitializer &= ofValue == predicted;
            predicted = ofValue;
        }
        if (!predicted || !m_program.genericEntrypoints.contains(predicted.index()))
            return std::nullopt;
        return KnownCallee { predicted, value, { }, false, false, { }, isClassFieldInitializer };
    }

    Arguments capturesOf(const KnownCallee& known, ValueID callee)
    {
        auto& types = m_program.at(known.function).captureTypes;
        Arguments captures;
        for (unsigned i = 0; i < types.size(); ++i) {
            if (i < known.captures.size()) {
                captures.append(known.captures[i]);
                continue;
            }
            Value capture = make(Opcode::GetCapture, types[i]);
            capture.children[0] = callee;
            capture.immediates = { known.function.index(), i };
            captures.append(append(WTF::move(capture)));
        }
        return captures;
    }

    void recordRefutedCallees()
    {
        m_refuted.fill(Refuted { }, m_function.blocks.size());
        m_classObjectFallbackBlocks.fill(LayoutID(), m_function.blocks.size());
        m_refutedShapes.fill(RefutedShape { }, m_function.blocks.size());
        m_pinnedFunctionBlocks.fill(PinnedFunction { }, m_function.blocks.size());
        for (auto& block : m_function.blocks) {
            if (block.terminator.kind != TerminatorKind::Branch)
                continue;
            auto& condition = m_function.at(block.terminator.value);
            if (condition.opcode == Opcode::IsClosureOf)
                m_refuted[block.terminator.targets[1].block.index()] = Refuted { originOf(condition.child(0)), condition.function() };
            if (condition.testsPinnedField()) {
                StringID name = m_program.at(condition.layout()).fields[condition.immediates[1]].name;
                m_pinnedFunctionBlocks[block.terminator.targets[0].block.index()] = PinnedFunction { condition.layout(), name, true };
                m_pinnedFunctionBlocks[block.terminator.targets[1].block.index()] = PinnedFunction { condition.layout(), name, false };
                continue;
            }
            if (condition.opcode == Opcode::IsClassObjectUnmodified)
                m_classObjectFallbackBlocks[block.terminator.targets[1].block.index()] = condition.layout();
            if (condition.opcode == Opcode::HasShapeOf && condition.immediates[1] != shapeTestIsExact)
                m_refutedShapes[block.terminator.targets[1].block.index()] = RefutedShape { originOf(condition.child(0)), condition.layout() };
            if (condition.opcode == Opcode::IsUndefinedOrNull)
                m_refuted[block.terminator.targets[0].block.index()] = Refuted { originOf(condition.child(0)), FunctionID() };
        }
    }

    bool isRefuted(BlockID block, const KnownCallee& known) const
    {
        if (m_refuted[block.index()] == Refuted { known.definition, known.function } || m_refuted[block.index()] == Refuted { known.definition, FunctionID() })
            return true;
        FunctionID bound = m_program.boundFunction(known.function, 0);
        return bound && m_refuted[block.index()] == Refuted { known.definition, bound };
    }

    BlockID testClosure(ValueID callee, const KnownCallee& known)
    {
        if (known.isCertainUnlessNullish) {
            Value test = make(Opcode::IsUndefinedOrNull, m_program.types.boolean);
            test.children[0] = callee;
            BlockID isClosure = branch(append(WTF::move(test)));
            BlockID isNil = m_block;
            m_refuted[isNil.index()] = Refuted { known.definition, FunctionID() };
            moveTo(isClosure);
            return isNil;
        }
        Value test = make(Opcode::IsClosureOf, m_program.types.boolean);
        test.children[0] = callee;
        test.immediates[0] = known.function.index();
        if (known.other || m_program.at(known.function).closuresHaveOwnProperties)
            test.immediates[1] = closureMayBeModified;
        BlockID otherwise = branch(append(WTF::move(test)));
        m_refuted[otherwise.index()] = Refuted { known.definition, known.function };
        return otherwise;
    }

    bool resolveCall(BlockID block, unsigned index)
    {
        Value original = m_function.at(m_function.at(block).values[index]);
        auto known = knownCallee(original.child(0));
        if (!known) {
            if (FunctionID method = predictedMethodOf(originOf(original.child(0))); method && m_program.boundFunction(method, 0))
                known = KnownCallee { method, originOf(original.child(0)), { }, false };
        }
        if (!known || !mayBeCalledDirectlyWithClosure(known->function) || isRefuted(block, *known))
            return false;
        if (known->other && !mayBeCalledDirectlyWithClosure(known->other))
            return false;
        ValueID id = m_function.at(block).values[index];
        Vector<ValueID, 2> literals;
        if (!known->isCertain)
            literals = literalsPassedToRecordParameters(id, known->function);
        if (!known->isCertain && std::get<0>(m_pinnedFunctionBlocks[block.index()]) && !std::get<2>(m_pinnedFunctionBlocks[block.index()]))
            return false;
        if (!m_mayGuess && !known->isCertain && !known->isCertainUnlessNullish && !known->other && !known->isClassFieldInitializer && literals.isEmpty())
            return false;
        open(block, index);
        BlockID otherwise;
        if (!known->isCertain) {
            absorbResultCast(id);
            otherwise = testClosure(original.child(0), *known);
        }
        FunctionID called = m_program.at(known->function).rampForClosures ? m_program.at(known->function).rampForClosures : known->function;
        Arguments given;
        if (m_program.at(called).takesCalleeClosure)
            given.append(original.child(0));
        given.append(original.arguments.span());
        m_unsharedLiterals = WTF::move(literals);
        callAndFinish(called, capturesOf(*known, original.child(0)).span(), original.child(1), given.span(), original.type, Yields::Result);
        m_unsharedLiterals.clear();
        if (otherwise && known->other) {
            moveTo(otherwise);
            KnownCallee second { known->other, known->definition, { }, false, true };
            BlockID isNil = testClosure(original.child(0), second);
            FunctionID calledIndirectly = m_program.at(second.function).rampForClosures ? m_program.at(second.function).rampForClosures : second.function;
            Arguments fallbackArguments;
            if (m_program.at(calledIndirectly).takesCalleeClosure)
                fallbackArguments.append(original.child(0));
            fallbackArguments.append(original.arguments.span());
            callAndFinish(calledIndirectly, capturesOf(second, original.child(0)).span(), original.child(1), fallbackArguments.span(), original.type, Yields::Result);
            moveTo(isNil);
            original.arguments.clear();
            appendThrowing(WTF::move(original));
            finishThrowing();
            return true;
        }
        if (otherwise) {
            moveTo(otherwise);
            if (FunctionID bound = known->isCertainUnlessNullish ? FunctionID() : m_program.boundFunction(known->function, 0); bound && m_program.at(bound).kind == FunctionKind::Bound) {
                KnownCallee second { bound, known->definition, { }, false };
                BlockID neither = testClosure(original.child(0), second);
                callAndFinish(bound, capturesOf(second, original.child(0)).span(), original.child(1), original.arguments.span(), original.type, Yields::Result);
                moveTo(neither);
            }
            if (known->isCertainUnlessNullish)
                original.arguments.clear();
            ValueID general = appendThrowing(WTF::move(original));
            if (known->isCertainUnlessNullish)
                finishThrowing();
            else
                finishWithCast(general);
        }
        return true;
    }

    bool resolveCallNoInline(BlockID block, unsigned index)
    {
        auto& value = m_function.at(m_function.at(block).values[index]);
        if (m_program.operationOf(value) != Operations::CallNoInline || value.arguments.size() < 2)
            return false;
        auto known = knownCallee(value.arguments[0]);
        if (!known || !known->isCertain || !mayBeCalledDirectly(known->function))
            return false;
        Arguments passed;
        passed.append(value.arguments.span().subspan(2));
        value.opcode = Opcode::Call;
        value.children = { value.arguments[0], value.arguments[1], ValueID() };
        value.immediates = { Format::none, nonInlinedDepth };
        value.arguments = WTF::move(passed);
        return resolveCall(block, index);
    }

    static unsigned boundArgumentCount(const Value& bind) { return bind.arguments.isEmpty() ? 0 : bind.arguments.size() - 1; }

    FunctionID predictedMethodOf(ValueID read) const
    {
        auto& definition = m_function.at(read);
        if (definition.opcode != Opcode::GetById)
            return { };
        LayoutID supertype = typeOf(definition.child(0)).singleLayout();
        if (!supertype || !isObjectLayout(m_program.at(supertype).kind))
            return { };
        FunctionID expected;
        for (LayoutID subtype : m_program.at(supertype).subtypes) {
            FunctionID method = m_program.at(subtype).kind == LayoutKind::Instance ? findMethod(m_program, subtype, definition.string(), MethodKind::Method) : FunctionID();
            if (!method || (expected && expected != method))
                return { };
            expected = method;
        }
        return expected;
    }

    std::optional<KnownCallee> boundCalleeOf(const Value& bind) const
    {
        static const BuiltinName name = Builtins::singleton().nameOf(asBytes("bind"_s.span8()));
        if (bind.opcode != Opcode::CallMethod || m_program.builtinName(bind.string()) != name || boundArgumentCount(bind) > Program::maxBoundArguments)
            return std::nullopt;
        auto known = knownCallee(bind.child(0));
        if (!known) {
            FunctionID method = predictedMethodOf(originOf(bind.child(0)));
            if (!method || !m_program.genericEntrypoints.contains(method.index()))
                return std::nullopt;
            known = KnownCallee { method, originOf(bind.child(0)), { }, false };
        }
        auto& callee = m_program.at(known->function);
        if ((callee.kind != FunctionKind::Method && callee.kind != FunctionKind::Arrow) || callee.declaredAs & functionIsGenerator)
            return std::nullopt;
        if (!mayBeCalledDirectly(known->function) || callee.has(Format::FunctionHasRestParameter) || callee.has(Format::FunctionIsBuiltin))
            return std::nullopt;
        unsigned numberOfBoundArguments = std::min<unsigned>(boundArgumentCount(bind), callee.parameterTypes.size());
        if (callee.captureTypes.size() + 2 + numberOfBoundArguments > UINT8_MAX)
            return std::nullopt;
        if (callee.has(Format::FunctionHasThis) && (bind.arguments.isEmpty() ? !m_program.at(m_program.types.undefined).isSubsetOf(m_program.at(callee.thisType ? callee.thisType : m_program.types.anything), m_program) : !isSubsetOf(bind.arguments[0], callee.thisType)))
            return std::nullopt;
        for (unsigned i = 0; i < numberOfBoundArguments; ++i) {
            if (!isSubsetOf(bind.arguments[i + 1], callee.parameterTypes[i]))
                return std::nullopt;
        }
        return known;
    }

    bool resolveBind(BlockID block, unsigned index)
    {
        Value original = m_function.at(m_function.at(block).values[index]);
        auto known = boundCalleeOf(original);
        if (!known || isRefuted(block, *known))
            return false;
        FunctionID bound = m_program.boundFunction(known->function, boundArgumentCount(original));
        if (!bound)
            return false;
        auto& callee = m_program.at(known->function);
        open(block, index);
        Value test = make(Opcode::IsClosureOf, m_program.types.boolean);
        test.children[0] = original.child(0);
        test.immediates = { known->function.index(), closureIsUnmodified };
        BlockID otherwise = branch(append(WTF::move(test)));
        m_refuted[otherwise.index()] = Refuted { known->definition, known->function };

        Value closure = make(Opcode::NewClosure, m_program.types.function);
        closure.immediates[0] = bound.index();
        closure.arguments = capturesOf(*known, original.child(0));
        if (callee.has(Format::FunctionHasThis))
            closure.arguments.append(original.arguments.isEmpty() ? append(make(Opcode::Undefined, m_program.types.undefined)) : original.arguments[0]);
        for (unsigned i = 0; i < boundArgumentCount(original) && i < callee.parameterTypes.size(); ++i)
            closure.arguments.append(original.arguments[i + 1]);
        if (closure.arguments.size() < m_program.at(bound).captureTypes.size())
            closure.arguments.append(original.child(0));
        finish(held(append(WTF::move(closure)), original.type, CastReason::Claim));

        moveTo(otherwise);
        finish(appendThrowing(WTF::move(original)));
        return true;
    }

    bool isReadFromGlobal(ValueID value) const
    {
        value = originOf(value);
        while (m_function.at(value).opcode == Opcode::GetById)
            value = originOf(m_function.at(value).child(0));
        auto& root = m_function.at(value);
        return root.opcode == Opcode::GetGlobal && !(m_program.mayRunInMultipleInstances && m_program.isGlobalThisName(root.string()));
    }

    bool hasGuard(ValueID method) const
    {
        for (auto& value : m_function.values) {
            if (value.opcode == Opcode::IsBuiltinFunction && value.child(0) == method)
                return true;
        }
        return false;
    }

    TypeID builtinResultType(BuiltinID first) const
    {
        auto& builtins = Builtins::singleton();
        uint16_t kinds = 0;
        uint16_t engineClass = 0;
        for (BuiltinID entry = first; entry; entry = builtins.next(entry)) {
            auto& result = builtins.at(entry).result;
            if (result.isAnything() || result.arguments || (kinds & BuiltinResult::Instance && result.engineClass != engineClass))
                return { };
            kinds |= result.kinds;
            engineClass = result.engineClass;
        }
        if (kinds == BuiltinResult::Boolean)
            return m_program.types.boolean;
        if (kinds && !(kinds & ~(BuiltinResult::Integer | BuiltinResult::Number)))
            return m_program.types.number;
        if (kinds == BuiltinResult::String)
            return m_program.types.string;
        if (kinds == BuiltinResult::Instance)
            return m_program.engineClassTypes[engineClass];
        return { };
    }

    struct PredictedBuiltinCallee {
        BuiltinPath path;
        BuiltinName global;
        BuiltinID entry;
        BuiltinID called;
    };

    std::optional<PredictedBuiltinCallee> builtinCalleeAt(const Value& site, const BuiltinPath& path) const
    {
        if (!path.global || (path.isOnPrototype && !path.member))
            return std::nullopt;
        auto& builtins = Builtins::singleton();
        BuiltinName global = m_program.builtinName(path.global);
        BuiltinID entry;
        if (path.member) {
            BuiltinName member = m_program.builtinName(path.member);
            entry = path.isOnPrototype ? builtins.findForInstance(BuiltinKind::Method, *builtins.classNamed(global), member) : builtins.findStatic(BuiltinKind::StaticFunction, global, member);
        }
        BuiltinID called = entry;
        switch (site.opcode) {
        case Opcode::CallMethod: {
            BuiltinName name = m_program.builtinName(site.string());
            if (!entry)
                called = builtins.findStatic(BuiltinKind::StaticFunction, global, name);
            else if (!builtins.callOrApply(name))
                return std::nullopt;
            break;
        }
        case Opcode::Call:
            if (!entry)
                called = builtins.find(BuiltinKind::Function, { }, global);
            break;
        case Opcode::Construct:
            called = entry ? BuiltinID() : builtins.findConstructor(global);
            break;
        default:
            return std::nullopt;
        }
        if (!called)
            return std::nullopt;
        return PredictedBuiltinCallee { path, global, entry, called };
    }

    bool isCalledLikeStaticMethod(const Value& site, const PredictedBuiltinCallee& callee) const
    {
        if (site.opcode != Opcode::Call || !callee.entry || callee.path.isOnPrototype)
            return false;
        auto& builtins = Builtins::singleton();
        if (builtins.classNamed(callee.global) == &builtins.promiseClass() || builtins.at(callee.entry).holderName == BuiltinName(AOT_TYPED_BUILTIN_NAME_TypedArray))
            return false;
        if (!site.child(1) || typeOf(site.child(1)).isUndefinedOrNull())
            return true;
        auto& read = m_function.at(originOf(site.child(0)));
        return m_program.mayRunInMultipleInstances && builtins.classNamed(callee.global) != &builtins.arrayClass() && read.opcode == Opcode::GetById && originOf(read.child(0)) == originOf(site.child(1));
    }

    bool isLiteralMethodCalledOnLiteral(const Value& site) const
    {
        if (site.opcode != Opcode::Call || !site.child(1))
            return false;
        auto& read = m_function.at(originOf(site.child(0)));
        if (read.opcode != Opcode::GetById)
            return false;
        ValueID holder = originOf(read.child(0));
        Opcode opcode = m_function.at(holder).opcode;
        return (opcode == Opcode::NewArray || opcode == Opcode::NewObject) && holder == originOf(site.child(1));
    }

    bool resolvePredictedBuiltinCall(BlockID block, unsigned index)
    {
        ValueID id = m_function.at(block).values[index];
        Value original = m_function.at(id);
        ValueID operand = original.child(0);
        if (isReadFromGlobal(operand) || isLiteralMethodCalledOnLiteral(original))
            return false;
        PredictedBuiltins expected = m_program.predictedBuiltins(m_function, operand);
        std::optional<StringID> prototype = m_program.find(asBytes("prototype"_s.span8()));
        Vector<PredictedBuiltinCallee, 2> callees;
        for (auto* path : { &expected.first, &expected.second }) {
            if (path->isOnPrototype && !prototype)
                continue;
            if (auto callee = builtinCalleeAt(original, *path))
                callees.append(*callee);
        }
        if (callees.isEmpty() || hasGuard(operand))
            return false;

        open(block, index);
        if (auto& joined = m_function.at(id); !joined.prediction() && !joined.predictedClosure()) {
            joined.setPrediction(builtinResultType(callees[0].called));
            if (callees.size() > 1 && builtinResultType(callees[1].called) != joined.prediction())
                joined.setPrediction({ });
        }
        auto appendRead = [&](ValueID object, StringID name) {
            Value read = make(Opcode::GetById, m_program.types.anything);
            read.children[0] = object;
            read.immediates[0] = name.index();
            return appendThrowing(WTF::move(read));
        };
        for (auto& callee : callees) {
            Value guard = make(Opcode::IsBuiltinFunction, m_program.types.boolean);
            guard.children[0] = operand;
            if (callee.entry)
                guard.immediates[0] = callee.entry.index();
            else
                guard.immediates[1] = callee.global.index();
            BlockID otherwise = branch(append(WTF::move(guard)));

            ValueID builtin = appendGlobal(callee.path.global);
            Value direct = original;
            if (isCalledLikeStaticMethod(original, callee)) {
                direct.opcode = Opcode::CallMethod;
                direct.children = { builtin, ValueID(), ValueID() };
                direct.immediates[0] = callee.path.member.index();
            } else {
                if (callee.path.isOnPrototype)
                    builtin = appendRead(builtin, *prototype);
                if (callee.path.member)
                    builtin = appendRead(builtin, callee.path.member);
                direct.children[0] = builtin;
                if (m_program.mayRunInMultipleInstances && original.opcode == Opcode::Call && !callee.entry)
                    direct.children[1] = append(make(Opcode::Undefined, m_program.types.undefined));
            }
            finish(appendThrowing(WTF::move(direct)));
            moveTo(otherwise);
        }
        finish(appendThrowing(WTF::move(original)));
        return true;
    }

    bool resolveCallOrApply(BlockID block, unsigned index)
    {
        Value original = m_function.at(m_function.at(block).values[index]);
        ValueID function = original.child(0);
        ValueID method;
        StringID name = original.string();
        if (original.opcode == Opcode::Call) {
            method = original.child(0);
            function = original.child(1);
            auto& read = m_function.at(method);
            if (read.opcode != Opcode::GetById || originOf(read.child(0)) != originOf(function))
                return false;
            name = read.string();
        }
        auto& builtins = Builtins::singleton();
        BuiltinID entry = builtins.callOrApply(m_program.builtinName(name));
        if (!entry || !(typeOf(function).anyFunction || typeOf(function).anyObject))
            return false;
        if (isReadFromGlobal(function) || (method && hasGuard(method)))
            return false;

        open(block, index);
        if (!method) {
            Value read = make(Opcode::GetById, m_program.types.anything);
            read.children[0] = function;
            read.immediates[0] = name.index();
            method = appendThrowing(WTF::move(read));
        }
        Value guard = make(Opcode::IsBuiltinFunction, m_program.types.boolean);
        guard.children[0] = method;
        guard.immediates[0] = entry.index();
        BlockID otherwise = branch(append(WTF::move(guard)));

        auto given = original.arguments.span();
        ValueID thisValue = given.empty() ? append(make(Opcode::Undefined, m_program.types.undefined)) : given[0];
        bool isApply = m_program.builtinName(name) == builtins.applyName();
        Value direct = make(Opcode::Call, original.type);
        if (isApply && given.size() > 1 && !typeOf(given[1]).isUndefinedOrNull()) {
            direct.opcode = Opcode::CallBuiltin;
            direct.immediates[0] = Operations::Apply.index();
            direct.arguments = Arguments { function, thisValue, given[1] };
        } else {
            direct.children = { function, thisValue, ValueID() };
            direct.immediates[1] = m_inliningDepth;
            if (!isApply && given.size() > 1)
                direct.arguments.append(given.subspan(1));
        }
        finish(appendThrowing(WTF::move(direct)));

        moveTo(otherwise);
        Value general = make(Opcode::Call, original.type);
        general.children = { method, function, ValueID() };
        general.arguments = WTF::move(original.arguments);
        finish(appendThrowing(WTF::move(general)));
        return true;
    }

    bool mayConstructDirectly(FunctionID id, bool allowsManyClosures = false) const
    {
        auto& callee = m_program.at(id);
        if (!mayBeCalledDirectly(id) || callee.kind != FunctionKind::Plain || !callee.captureTypes.isEmpty() || (callee.hasManyClosures && !allowsManyClosures) || !callee.has(Format::FunctionHasThis) || callee.has(Format::FunctionHasNewTarget))
            return false;
        return !mayReturnObject(callee);
    }

    bool mayReturnObject(const Function& callee) const
    {
        for (auto& block : callee.blocks) {
            if (block.terminator.kind == TerminatorKind::Return && block.terminator.value && m_program.typeOf(callee, block.terminator.value).hasObjects())
                return true;
        }
        return false;
    }

    bool mayConstructClassDirectly(FunctionID id) const
    {
        auto& constructor = m_program.at(id);
        if (constructor.kind != FunctionKind::Constructor || constructor.blocks.isEmpty() || constructor.isGenericEntrypoint() || !constructor.constructedClass)
            return false;
        auto& ofClass = m_program.at(constructor.constructedClass);
        if (!ofClass.hasFixedLayout || !ofClass.isEvaluatedOnce || !ofClass.isAllocated)
            return false;
        if (!constructor.captureTypes.isEmpty() || !constructor.has(Format::FunctionHasThis) || constructor.has(Format::FunctionHasNewTarget | Format::FunctionHasRestParameter))
            return false;
        return !mayReturnObject(constructor);
    }

    bool constructsOrdinaryObject(FunctionID id) const
    {
        auto& callee = m_program.at(id);
        if (callee.has(Format::FunctionIsAsync | Format::FunctionIsGenerator) || callee.resumeFunction || callee.startFunction || callee.thisType)
            return false;
        if (callee.kind == FunctionKind::Plain)
            return !callee.has(Format::FunctionHasNewTarget) && mayBeCalledDirectly(id) && !m_program.mayUseSlimClosures(callee);
        if (callee.kind != FunctionKind::Constructor || callee.blocks.isEmpty() || callee.isGenericEntrypoint() || !callee.constructedClass || !canPassRestArguments(callee))
            return false;
        auto& ofClass = m_program.at(callee.constructedClass);
        return !ofClass.hasFixedLayout && !ofClass.base;
    }

    bool resolveOrdinaryObjectConstruct(BlockID block, unsigned index, const KnownCallee& known)
    {
        Value original = m_function.at(m_function.at(block).values[index]);
        auto& callee = m_program.at(known.function);
        open(block, index);
        BlockID otherwise;
        if (!known.isCertain)
            otherwise = testClosure(original.child(0), known);
        BlockID asks;
        for (LayoutID ofClass : classesWithResolvedSuper(known.function)) {
            Value test = make(Opcode::IsClassObjectUnmodified, m_program.types.boolean);
            test.immediates[0] = ofClass.index();
            BlockID isNot = branch(append(WTF::move(test)));
            m_classObjectFallbackBlocks[isNot.index()] = ofClass;
            if (!asks) {
                asks = isNot;
                continue;
            }
            m_function.at(isNot).terminator.kind = TerminatorKind::Jump;
            m_function.at(isNot).terminator.targets.append(Target { asks, { } });
        }
        Arguments captures = capturesOf(known, original.child(0));
        bool isAllocatedBySuperclass = callee.kind == FunctionKind::Constructor && m_program.at(callee.constructedClass).hasUntypedBase;
        Value object = make(isAllocatedBySuperclass ? Opcode::Undefined : Opcode::CreateThis, isAllocatedBySuperclass ? m_program.types.undefined : m_program.types.object);
        if (!isAllocatedBySuperclass)
            object.children[0] = original.child(0);
        ValueID created = append(WTF::move(object));
        Arguments given;
        if (callee.has(Format::FunctionHasNewTarget))
            given.append(original.child(0));
        given.appendVector(original.arguments);
        ValueID returned = call(known.function, passedTo(known.function, captures.span(), created, given.span()));
        m_function.at(returned).constructs = true;
        if (isAllocatedBySuperclass)
            created = returned;
        else if (mayReturnObject(callee)) {
            Value test = make(Opcode::CallBuiltin, m_program.types.boolean);
            test.immediates[0] = Operations::IsObject.index();
            test.arguments.append(returned);
            BlockID isNotObject = branch(append(WTF::move(test)));
            finish(held(returned, original.type, CastReason::Claim));
            moveTo(isNotObject);
        }
        finish(held(created, original.type, CastReason::Claim));
        if (asks) {
            moveTo(asks);
            finish(appendThrowing(Value(original)));
        }
        if (otherwise) {
            moveTo(otherwise);
            if (known.isCertainUnlessNullish)
                original.arguments.clear();
            ValueID general = appendThrowing(WTF::move(original));
            if (known.isCertainUnlessNullish)
                finishThrowing();
            else
                finish(general);
        }
        return true;
    }

    bool superIsResolved(LayoutID ofClass) const
    {
        FunctionID parent = m_program.at(ofClass).declaredSuperclassConstructor;
        return parent && constructsOrdinaryObject(parent) && m_program.at(parent).captureTypes.isEmpty();
    }

    Vector<LayoutID, 4> classesWithResolvedSuper(FunctionID constructor) const
    {
        static constexpr unsigned maxLevels = 8;
        Vector<LayoutID, 4> result;
        if (m_program.at(constructor).kind != FunctionKind::Constructor)
            return result;
        for (LayoutID ofClass = m_program.at(constructor).constructedClass; ofClass && superIsResolved(ofClass) && result.size() < maxLevels; ofClass = m_program.at(m_program.at(ofClass).declaredSuperclassConstructor).constructedClass)
            result.append(ofClass);
        return result;
    }

    bool isKnownUnmodified(BlockID block, unsigned index, LayoutID ofClass) const
    {
        for (unsigned steps = 0; steps < 64; ++steps) {
            for (unsigned i = index; i--;) {
                switch (m_function.at(m_function.at(block).values[i]).opcode) {
                case Opcode::CallDirect:
                case Opcode::Await:
                case Opcode::Yield:
                case Opcode::GetResumeMode:
                    return false;
                default:
                    break;
                }
                if (writesOf(m_program, m_function, m_function.at(block), i) > Writes::Fields)
                    return false;
            }
            BlockID predecessor;
            unsigned numberOfEdges = 0;
            for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
                for (auto& target : m_function.blocks[b].terminator.targets) {
                    if (target.block == block) {
                        predecessor = BlockID(b);
                        ++numberOfEdges;
                    }
                }
            }
            if (numberOfEdges != 1)
                return false;
            auto& terminator = m_function.at(predecessor).terminator;
            if (terminator.kind == TerminatorKind::Branch) {
                auto& condition = m_function.at(terminator.value);
                if (condition.opcode == Opcode::IsClassObjectUnmodified && !condition.testsPinnedField() && condition.layout() == ofClass)
                    return terminator.targets[0].block == block;
            } else if (terminator.kind != TerminatorKind::Jump)
                return false;
            block = predecessor;
            index = m_function.at(block).values.size();
        }
        return false;
    }

    bool resolveConstructSuper(BlockID block, unsigned index)
    {
        Value original = m_function.at(m_function.at(block).values[index]);
        LayoutID ofClass = classOfClassObject(original.child(0));
        if (!ofClass || m_classObjectFallbackBlocks[block.index()] == ofClass)
            return false;
        FunctionID parent = m_program.at(ofClass).declaredSuperclassConstructor;
        if (!parent || !constructsOrdinaryObject(parent) || !m_program.at(parent).captureTypes.isEmpty())
            return false;
        auto& callee = m_program.at(parent);
        ValueID newTarget = original.child(1);
        bool isAllocatedBySuperclass = m_program.at(callee.constructedClass).hasUntypedBase;
        if (!isAllocatedBySuperclass && m_function.at(originOf(newTarget)).opcode != Opcode::FunctionReference)
            return false;
        bool isUnmodified = isKnownUnmodified(block, index, ofClass);
        open(block, index);
        if (!isUnmodified)
            guardClassObjectUnmodified(ofClass, original);
        ValueID created;
        if (isAllocatedBySuperclass)
            created = append(make(Opcode::Undefined, m_program.types.undefined));
        else {
            Value constructor = make(Opcode::FunctionReference, m_program.types.function);
            constructor.immediates[0] = parent.index();
            Value object = make(Opcode::CreateThis, m_program.types.object);
            object.children[0] = newTarget;
            object.children[1] = append(WTF::move(constructor));
            created = append(WTF::move(object));
        }
        Arguments given;
        if (callee.has(Format::FunctionHasNewTarget))
            given.append(newTarget);
        given.appendVector(original.arguments);
        ValueID returned = call(parent, passedTo(parent, { }, created, given.span()));
        m_function.at(returned).constructs = true;
        if (isAllocatedBySuperclass)
            created = returned;
        else if (mayReturnObject(callee)) {
            Value test = make(Opcode::CallBuiltin, m_program.types.boolean);
            test.immediates[0] = Operations::IsObject.index();
            test.arguments.append(returned);
            BlockID isNotObject = branch(append(WTF::move(test)));
            finish(held(returned, original.type, CastReason::Claim));
            moveTo(isNotObject);
        }
        finish(held(created, original.type, CastReason::Claim));
        return true;
    }

    bool isUsedOnlyBy(ValueID value, ValueID user) const
    {
        bool isUsedElsewhere = false;
        for (auto& block : m_function.blocks) {
            for (ValueID id : block.values) {
                if (id == user)
                    continue;
                m_function.at(id).forEachChild([&](ValueID child) {
                    isUsedElsewhere |= child == value;
                });
            }
            isUsedElsewhere |= block.terminator.value == value;
            for (auto& target : block.terminator.targets)
                isUsedElsewhere |= target.arguments.contains(value);
        }
        return !isUsedElsewhere;
    }

    bool resolveConstructOfUnknownCallee(BlockID block, unsigned index)
    {
        ValueID id = m_function.at(block).values[index];
        Value original = m_function.at(id);
        if (original.isOnSlowPath)
            return false;
        ValueID object = original.arguments[1];
        if (m_function.at(object).opcode != Opcode::NewObjectWithShape)
            return false;
        size_t allocationSite = m_function.at(block).values.find(object);
        if (allocationSite == notFound || allocationSite >= index || !isUsedOnlyBy(object, id))
            return false;
        Vector<FunctionID, maxCallees> takers;
        for (FunctionID candidate : m_program.constructorFunctionsWithTypedThis) {
            auto& function = m_program.at(candidate);
            if (!isSubsetOf(object, function.assertedThisType ? function.assertedThisType : function.thisType))
                continue;
            if (takers.size() == maxCallees)
                return false;
            takers.append(candidate);
        }
        m_function.at(block).values.removeAt(allocationSite);

        open(block, index - 1);
        if (!takers.isEmpty())
            absorbResultCast(id);
        BlockID takesObject = takers.isEmpty() ? BlockID() : addBlock();
        for (FunctionID taker : takers) {
            Value test = make(Opcode::IsClosureOf, m_program.types.boolean);
            test.children[0] = original.arguments[0];
            test.immediates = { taker.index(), closureMayBeModified };
            BlockID otherwise = branch(append(WTF::move(test)));
            auto& terminator = m_function.at(m_block).terminator;
            terminator.kind = TerminatorKind::Jump;
            terminator.targets.append(Target { takesObject, { } });
            moveTo(otherwise);
        }
        Value construct = make(Opcode::Construct, m_program.types.anything);
        construct.children[0] = original.arguments[0];
        construct.arguments.append(original.arguments.span().subspan(2));
        finishWithCast(held(appendThrowing(WTF::move(construct)), original.type, CastReason::Claim));
        if (!takers.isEmpty()) {
            moveTo(takesObject);
            place(object);
            original.isOnSlowPath = true;
            finishWithCast(appendThrowing(WTF::move(original)));
        }
        return true;
    }

    bool resolveConstruct(BlockID block, unsigned index)
    {
        ValueID id = m_function.at(block).values[index];
        Value original = m_function.at(id);
        if (m_program.operationOf(original) != Operations::ConstructWith || original.arguments.size() < 2)
            return false;
        auto known = knownCallee(original.arguments[0]);
        if (known && isRefuted(block, *known))
            return false;
        if (!known || !mayConstructDirectly(known->function, true))
            return resolveConstructOfUnknownCallee(block, index);
        bool isOneOfMany = m_program.at(known->function).hasManyClosures;
        if (isOneOfMany && !m_program.genericEntrypoints.contains(known->function.index()))
            return resolveConstructOfUnknownCallee(block, index);
        ValueID object = original.arguments[1];
        if (m_function.at(object).opcode != Opcode::NewObjectWithShape || m_program.at(m_function.at(object).layout()).kind != LayoutKind::Record)
            return false;
        if (!m_program.at(known->function).thisType || !isSubsetOf(object, m_program.at(known->function).thisType))
            return resolveConstructOfUnknownCallee(block, index);
        size_t allocationSite = m_function.at(block).values.find(object);
        if (allocationSite == notFound || allocationSite >= index || !isUsedOnlyBy(object, id))
            return false;
        m_function.at(block).values.removeAt(allocationSite);

        open(block, index - 1);
        BlockID otherwise;
        if (isOneOfMany) {
            absorbResultCast(id);
            Value test = make(Opcode::IsClosureOf, m_program.types.boolean);
            test.children[0] = original.arguments[0];
            test.immediates = { known->function.index(), closureIsConstructingCallee };
            otherwise = branch(append(WTF::move(test)));
            m_refuted[otherwise.index()] = Refuted { known->definition, known->function };
        } else if (!known->isCertain) {
            absorbResultCast(id);
            otherwise = testClosure(original.arguments[0], *known);
        }
        Allocation allocation = m_function.allocations[m_function.at(object).allocation()];
        allocation.constructedBy = known->function;
        uint32_t number = m_function.allocationOf(WTF::move(allocation));
        ValueID created = otherwise ? addValue(Value(m_function.at(object))) : object;
        m_function.at(created).immediates[1] = number;
        Arguments arguments = passedTo(known->function, { }, created, original.arguments.span().subspan(2));
        place(created);
        call(known->function, WTF::move(arguments));
        finishWithCast(held(created, original.type, CastReason::Claim));
        if (otherwise) {
            moveTo(otherwise);
            place(object);
            bool isNil = known->isCertainUnlessNullish && !isOneOfMany;
            if (isNil)
                original.arguments.shrink(2);
            ValueID general = appendThrowing(WTF::move(original));
            if (isNil)
                finishThrowing();
            else
                finishWithCast(general);
        }
        return true;
    }

    bool resolveFunctionConstruct(BlockID block, unsigned index)
    {
        ValueID id = m_function.at(block).values[index];
        Value original = m_function.at(id);
        auto known = knownCallee(original.child(0));
        if (!known || isRefuted(block, *known))
            return false;
        auto& callee = m_program.at(known->function);
        if (LayoutID asked = m_classObjectFallbackBlocks[block.index()]; asked && classesWithResolvedSuper(known->function).contains(asked))
            return false;
        Allocation allocation;
        if (mayConstructClassDirectly(known->function)) {
            allocation.layout = callee.constructedClass;
            setInitializedFields(m_program.at(allocation.layout), allocation);
        } else if (mayConstructDirectly(known->function) && callee.thisType && callee.allocatesForUnknownCallers) {
            allocation.layout = m_program.at(callee.thisType).singleLayout();
            allocation.constructedBy = known->function;
            if (!allocation.layout || m_program.at(allocation.layout).kind != LayoutKind::Record || !m_program.at(allocation.layout).isAllocated)
                return false;
        } else if (constructsOrdinaryObject(known->function))
            return resolveOrdinaryObjectConstruct(block, index, *known);
        else
            return false;

        open(block, index);
        BlockID otherwise;
        if (!known->isCertain) {
            absorbResultCast(id);
            otherwise = testClosure(original.child(0), *known);
        }
        Value object = make(Opcode::NewObjectWithShape, m_program.at(allocation.layout).type);
        object.immediates[0] = allocation.layout.index();
        object.immediates[1] = m_function.allocationOf(WTF::move(allocation));
        ValueID created = append(WTF::move(object));
        call(known->function, passedTo(known->function, { }, created, original.arguments.span()));
        finishWithCast(held(created, original.type, CastReason::Claim));
        if (otherwise) {
            moveTo(otherwise);
            if (known->isCertainUnlessNullish)
                original.arguments.clear();
            ValueID general = appendThrowing(WTF::move(original));
            if (known->isCertainUnlessNullish)
                finishThrowing();
            else
                finishWithCast(general);
        }
        return true;
    }

    const Program& m_program;
    Function& m_function;
    bool m_mayGuess;
    Vector<ValueID, 2> m_unsharedLiterals;
    StringID m_usingNeedsObjectMessage;
    StringID m_usingNeedsDisposeMessage;
    StringID m_symbol;
    StringID m_dispose;
    StringID m_typeError;
    StringID m_suppressedError;
    Vector<uint32_t> m_numberOfUses;
    unsigned m_originalBlockCount { 0 };
    BitVector m_isReached;
    UncheckedKeyHashMap<unsigned, Vector<unsigned, 1>, IntHash<unsigned>, WTF::UnsignedWithZeroKeyHashTraits<unsigned>> m_unreachablePredecessors;
    Vector<unsigned> m_indexInRegion;
    Vector<Refuted> m_refuted;
    Vector<LayoutID> m_classObjectFallbackBlocks;
    Vector<RefutedShape> m_refutedShapes;
    Vector<PinnedFunction> m_pinnedFunctionBlocks;

    uint32_t m_position { 0 };
    uint32_t m_inlineFrame { 0 };
    bool m_isInTailPosition { false };
    uint32_t m_inliningDepth { Format::none };
    bool m_hasHandler { false };
    Target m_handler;
    BlockID m_after;
    std::optional<Value> m_resultCast;
    BlockID m_block;
    bool m_lastMayThrow { false };
};

} // anonymous namespace

unsigned resolveCalls(const Program& program, Function& function, bool mayGuess, CleanUpTiming timing)
{
    return CallResolution(program, function, mayGuess).run(timing);
}

static Function createGenericEntrypoint(const Program& program, FunctionID id, FunctionID written = { })
{
    FunctionID untypedClone = written ? program.at(id).untypedClone : FunctionID();
    if (untypedClone) {
        auto& served = program.at(id);
        auto& second = program.at(untypedClone);
        if (second.captureTypes == served.captureTypes && second.parameterTypes.size() == served.parameterTypes.size() && second.has(Format::FunctionHasThis) == served.has(Format::FunctionHasThis) && !served.has(Format::FunctionHasRestParameter) && !served.takesCalleeClosure && !second.takesCalleeClosure)
            written = { };
        else
            untypedClone = { };
    }
    FunctionID called = written ? written : program.at(id).rampForClosures ? program.at(id).rampForClosures : id;
    auto& callee = program.at(called);
    if (written) {
        auto& served = program.at(id);
        RELEASE_ASSERT(callee.kind == served.kind && callee.length == served.length && callee.captureTypes == served.captureTypes && callee.parameterTypes.size() == served.parameterTypes.size());
        RELEASE_ASSERT(callee.has(Format::FunctionHasThis) == served.has(Format::FunctionHasThis) && callee.has(Format::FunctionHasRestParameter) == served.has(Format::FunctionHasRestParameter) && !callee.takesCalleeClosure && !served.takesCalleeClosure);
    }
    auto& types = program.types;
    Function entry;
    entry.name = callee.name;
    entry.kind = callee.kind;
    entry.constructedClass = callee.constructedClass;
    entry.declaredAs = written ? program.at(id).declaredAs : callee.declaredAs;
    entry.module = callee.module;
    entry.position = callee.position;
    entry.length = callee.length;
    entry.flags = FunctionIsGenericEntrypoint | Format::FunctionHasThis | (callee.flags & Format::FunctionIsTyped);
    entry.captureTypes.append(program.at(program.closureLayout).type);
    unsigned first = callee.takesCalleeClosure;
    bool usesArgumentBuffer = callee.has(Format::FunctionHasRestParameter) || callee.parameterTypes.size() - first > numberOfArgumentGPRs;
    if (usesArgumentBuffer)
        entry.flags |= FunctionUsesArgumentBuffer;
    entry.parameterTypes.fill(TypeID(), callee.parameterTypes.size() - first);
    entry.blocks.append(Block { });

    auto make = [&](Opcode opcode, TypeID type) {
        Value value;
        value.opcode = opcode;
        value.position = callee.position;
        value.type = type;
        value.rep = repOf(program.at(type));
        return value;
    };
    unsigned current = 0;
    bool isWatched = false;
    auto append = [&](Value&& value) {
        ValueID result(entry.values.size());
        entry.values.append(WTF::move(value));
        entry.blocks[current].values.append(result);
        return result;
    };
    auto addParameter = [&](TypeID type) {
        ValueID result(entry.values.size());
        Value value = make(Opcode::Parameter, type);
        value.immediates = { 0, static_cast<uint32_t>(entry.blocks[0].parameters.size()) };
        entry.values.append(WTF::move(value));
        entry.blocks[0].parameters.append(result);
        return result;
    };
    auto held = [&](ValueID value, TypeID type, TypeID asserted, CastReason reason) {
        if (!asserted && (!type || program.castIsRedundant(program.typeOf(entry, value), program.at(type), reason)))
            return value;
        Value cast = make(Opcode::Cast, type ? type : types.anything);
        cast.setAssertedType(asserted);
        cast.children[0] = value;
        cast.immediates[1] = static_cast<uint32_t>(reason);
        bool acceptsUntypedByDeclaration = type && program.at(type).acceptsUntypedObjects(program) && !program.isRelaxedForLogging(program.at(type));
        if (!untypedClone || asserted || !type || (reason == CastReason::As && !program.at(type).objectLayouts.isEmpty() && !(program.logsTypeErrors && acceptsUntypedByDeclaration)))
            return append(WTF::move(cast));
        cast.immediates[1] = static_cast<uint32_t>(CastReason::Claim);
        ValueID result = append(WTF::move(cast));
        if (!isWatched) {
            isWatched = true;
            entry.blocks.append(Block { });
        }
        entry.blocks.append(Block { });
        auto& terminator = entry.blocks[current].terminator;
        terminator.kind = TerminatorKind::Invoke;
        current = entry.blocks.size() - 1;
        terminator.targets.append(Target { BlockID(current), { } });
        terminator.targets.append(Target { BlockID(1u), { } });
        return result;
    };

    ValueID closure = addParameter(program.at(program.closureLayout).type);
    ValueID thisValue = addParameter(types.anything);
    Vector<ValueID> given;
    for (unsigned i = first; i < callee.parameterTypes.size(); ++i) {
        if (!usesArgumentBuffer) {
            given.append(addParameter(types.anything));
            continue;
        }
        bool isRest = callee.has(Format::FunctionHasRestParameter) && i + 1 == callee.parameterTypes.size();
        TypeID type = types.anything;
        if (TypeID declared = callee.parameterTypes[i]; isRest && declared && Type::engineClass(Builtins::singleton().arrayClass()).isSubsetOf(program.at(declared), program))
            type = declared;
        Value passed = make(isRest ? Opcode::CreateRest : Opcode::GetArgument, type);
        passed.immediates[0] = i - first;
        given.append(append(WTF::move(passed)));
    }
    Arguments all;
    for (unsigned i = 0; i < callee.captureTypes.size(); ++i) {
        Value capture = make(Opcode::GetCapture, callee.captureTypes[i]);
        capture.children[0] = closure;
        capture.immediates = { id.index(), i };
        all.append(append(WTF::move(capture)));
    }
    if (callee.has(Format::FunctionHasThis))
        all.append(held(thisValue, callee.thisType, callee.assertedThisType, CastReason::Store));
    if (callee.takesCalleeClosure)
        all.append(closure);
    for (unsigned i = 0; i < given.size(); ++i) {
        bool isAllocatedHere = entry.at(given[i]).opcode == Opcode::CreateRest;
        all.append(held(given[i], callee.parameterTypes[first + i], isAllocatedHere ? TypeID() : callee.assertedParameterType(first + i), callee.parameterCastReason(first + i)));
    }
    Value call = make(Opcode::CallDirect, callee.resultType ? callee.resultType : types.anything);
    call.immediates[0] = called.index();
    call.arguments = WTF::move(all);
    entry.blocks[current].terminator.kind = TerminatorKind::Return;
    entry.blocks[current].terminator.value = append(WTF::move(call));
    if (!isWatched)
        return entry;
    entry.flags |= FunctionLogsTypeMismatches;
    current = 1;
    append(make(Opcode::CatchValue, types.anything));
    auto& second = program.at(untypedClone);
    Arguments original;
    for (unsigned i = 0; i < second.captureTypes.size(); ++i) {
        Value capture = make(Opcode::GetCapture, second.captureTypes[i]);
        capture.children[0] = closure;
        capture.immediates = { id.index(), i };
        original.append(append(WTF::move(capture)));
    }
    if (second.has(Format::FunctionHasThis))
        original.append(thisValue);
    for (unsigned i = 0; i < given.size(); ++i) {
        if (!usesArgumentBuffer) {
            original.append(given[i]);
            continue;
        }
        Value passed = make(Opcode::GetArgument, types.anything);
        passed.immediates[0] = i;
        original.append(append(WTF::move(passed)));
    }
    Value untypedCloneCall = make(Opcode::CallDirect, second.resultType ? second.resultType : types.anything);
    untypedCloneCall.immediates[0] = untypedClone.index();
    untypedCloneCall.arguments = WTF::move(original);
    entry.blocks[1].terminator.kind = TerminatorKind::Return;
    entry.blocks[1].terminator.value = append(WTF::move(untypedCloneCall));
    return entry;
}

namespace {

struct ValueEntrySignature {
    Vector<TypeID, 4> parameters;
    TypeID result;
};

static bool isPlainFunctionWithKnownCallers(const Program& program, FunctionID id)
{
    auto& function = program.at(id);
    if (function.kind != FunctionKind::Plain && function.kind != FunctionKind::Arrow)
        return false;
    if (function.blocks.isEmpty() || function.isGenericEntrypoint() || function.isSharedOutlinedBody || !function.resultTypes.isEmpty())
        return false;
    if (function.has(Format::FunctionHasRestParameter) || function.has(Format::FunctionHasNewTarget) || function.has(FunctionUsesArgumentBuffer) || function.has(FunctionHasUnknownCallers) || function.has(FunctionIsPartOfHiddenCoroutine) || function.has(FunctionLogsTypeMismatches) || function.has(FunctionIsEnteredByValue))
        return false;
    if (function.takesCalleeClosure || function.rampForClosures || function.isRampForClosures || function.declaredGenericEntrypoint || function.untypedClone || function.resumeFunction || function.startFunction || function.worker || function.declaredAs & (functionIsAsync | functionIsGenerator))
        return false;
    return !program.isResumeFunction(function) && !program.isEntry(id);
}

static bool canHaveValueEntry(const Program& program, FunctionID id)
{
    auto& function = program.at(id);
    if (!isPlainFunctionWithKnownCallers(program, id) || function.has(Format::FunctionIsBuiltin) || function.assertedThisType || !program.genericEntrypoints.contains(id.index()) || !program.usesSlimClosures(function))
        return false;
    unsigned numberOfGPRs = 0;
    unsigned numberOfFPRs = 0;
    auto count = [&](TypeID type) {
        Rep rep = repOf(program.at(type ? type : program.types.anything));
        ++(rep == Rep::Double || rep == Rep::OptionalDouble ? numberOfFPRs : numberOfGPRs);
    };
    for (TypeID type : function.captureTypes)
        count(type);
    if (function.has(Format::FunctionHasThis))
        count(function.thisType);
    for (TypeID type : function.parameterTypes)
        count(type);
    if (numberOfGPRs > numberOfDirectArgumentGPRs || numberOfFPRs > FPRInfo::numberOfArgumentRegisters)
        return false;
    for (unsigned i = 0; i < function.parameterTypes.size(); ++i) {
        if (function.assertedParameterType(i))
            return false;
    }
    return std::ranges::none_of(function.values, [&](const Value& value) {
        if (value.opcode == Opcode::GetMyArgumentByVal)
            return value.readsArgumentsObject();
        return (value.opcode == Opcode::Operation || value.opcode == Opcode::CallBuiltin) && languageOperationOf(program, value) == LanguageOperation::CreateArguments;
    });
}

static Value makeEntryValue(const Program& program, Opcode opcode, TypeID type, uint32_t position)
{
    Value value;
    value.opcode = opcode;
    value.position = position;
    value.type = type;
    value.rep = repOf(program.at(type));
    return value;
}

static ValueID addEntryParameter(const Program& program, Function& function, TypeID type)
{
    ValueID result(function.values.size());
    Value value = makeEntryValue(program, Opcode::Parameter, type, function.position);
    value.immediates = { 0, static_cast<uint32_t>(function.blocks[0].parameters.size()) };
    function.values.append(WTF::move(value));
    function.blocks[0].parameters.append(result);
    return result;
}

static Function createValueEntry(const Program& program, FunctionID id, const ValueEntrySignature& signature)
{
    auto& callee = program.at(id);
    Function entry;
    entry.name = callee.name;
    entry.kind = FunctionKind::Plain;
    entry.module = callee.module;
    entry.position = callee.position;
    entry.length = callee.length;
    entry.flags = Format::FunctionIsTyped | Format::FunctionIsHiddenInStackTraces;
    entry.isValueEntry = true;
    entry.valueEntryTarget = id;
    entry.resultType = signature.result ? signature.result : program.types.undefined;
    entry.returnsBoxedValue = repOf(program.at(entry.resultType)) == Rep::Int32;
    entry.blocks.append(Block { });
    auto append = [&](Value&& value) {
        ValueID result(entry.values.size());
        entry.values.append(WTF::move(value));
        entry.blocks[0].values.append(result);
        return result;
    };
    ValueID closure = addEntryParameter(program, entry, program.at(program.closureLayout).type);
    ValueID thisValue = addEntryParameter(program, entry, program.types.anything);
    Vector<ValueID, 8> given;
    for (TypeID type : signature.parameters)
        given.append(addEntryParameter(program, entry, type));
    auto undefinedValue = [&] {
        return append(makeEntryValue(program, Opcode::Undefined, program.types.undefined, callee.position));
    };
    auto held = [&](ValueID passed, TypeID type, CastReason reason) {
        if (!type || program.castIsRedundant(program.typeOf(entry, passed), program.at(type), reason))
            return passed;
        Value cast = makeEntryValue(program, Opcode::Cast, type, callee.position);
        cast.children[0] = passed;
        cast.immediates[1] = static_cast<uint32_t>(reason);
        return append(WTF::move(cast));
    };
    Arguments all;
    for (unsigned i = 0; i < callee.captureTypes.size(); ++i) {
        Value capture = makeEntryValue(program, Opcode::GetCapture, callee.captureTypes[i], callee.position);
        capture.children[0] = closure;
        capture.immediates = { id.index(), i };
        all.append(append(WTF::move(capture)));
    }
    if (callee.has(Format::FunctionHasThis))
        all.append(held(thisValue, callee.thisType, CastReason::Store));
    for (unsigned i = 0; i < callee.parameterTypes.size(); ++i)
        all.append(held(i < given.size() ? given[i] : undefinedValue(), callee.parameterTypes[i], callee.parameterCastReason(i)));
    Value call = makeEntryValue(program, Opcode::CallDirect, callee.resultType ? callee.resultType : program.types.anything, callee.position);
    call.immediates[0] = id.index();
    call.arguments = WTF::move(all);
    entry.blocks[0].terminator.kind = TerminatorKind::Return;
    entry.blocks[0].terminator.value = append(WTF::move(call));
    return entry;
}

static bool canHaveValueEntryClone(const Program& program, FunctionID id, const ValueEntrySignature& signature)
{
    static constexpr unsigned maxValueEntryCloneValues = 256;
    auto& callee = program.at(id);
    Rep rep = repOf(program.at(signature.result));
    if ((rep != Rep::Double && rep != Rep::OptionalDouble && rep != Rep::Int64 && rep != Rep::OptionalInt64) || !callee.resultType || repOf(program.at(callee.resultType)) != rep)
        return false;
    if (callee.values.size() > maxValueEntryCloneValues || callee.has(Format::FunctionHasThis) || callee.parameterTypes.size() < signature.parameters.size() || callee.blocks[0].parameters.size() != callee.captureTypes.size() + callee.parameterTypes.size())
        return false;
    if (std::ranges::any_of(callee.parameterTypes, [](TypeID type) { return !type; }))
        return false;
    return std::ranges::none_of(callee.values, [&](const Value& value) {
        return value.isInTailPosition || value.opcode == Opcode::NewClosure || value.opcode == Opcode::NewClosureOnce || value.opcode == Opcode::FunctionReference || (value.opcode == Opcode::CallDirect && value.function() == id);
    });
}

static Function createValueEntryClone(const Program& program, FunctionID id, const ValueEntrySignature& signature)
{
    Function twin = program.at(id);
    twin.isValueEntry = true;
    twin.isValueEntryClone = true;
    twin.valueEntryTarget = id;
    twin.takesCapturesFromClosure = !twin.captureTypes.isEmpty();
    auto& first = twin.blocks[0];
    unsigned numberOfCaptures = twin.captureTypes.size();
    unsigned numberAtHead = 0;
    while (twin.parameterTypes.size() > signature.parameters.size()) {
        ValueID parameter = first.parameters.takeLast();
        twin.parameterTypes.removeLast();
        twin.at(parameter) = makeEntryValue(program, Opcode::Undefined, program.types.undefined, twin.position);
        first.values.insert(numberAtHead++, parameter);
    }
    for (unsigned i = 0; i < signature.parameters.size(); ++i) {
        TypeID own = twin.parameterTypes[i];
        TypeID passed = signature.parameters[i];
        CastReason reason = twin.parameterCastReason(i);
        if (repOf(program.at(own)) == repOf(program.at(passed)) && program.castIsRedundant(program.at(passed), program.at(own), reason))
            continue;
        ValueID parameter = first.parameters[numberOfCaptures + i];
        ValueID held(twin.values.size());
        auto replaceWithCast = [&](ValueID& child) {
            if (child == parameter)
                child = held;
        };
        for (auto& value : twin.values)
            value.forEachChild(replaceWithCast);
        for (auto& block : twin.blocks)
            block.terminator.forEachChild(replaceWithCast);
        Value cast = makeEntryValue(program, Opcode::Cast, own, twin.position);
        cast.children[0] = parameter;
        cast.immediates[1] = static_cast<uint32_t>(reason);
        twin.values.append(WTF::move(cast));
        twin.blocks[0].values.insert(numberAtHead++, held);
        twin.at(parameter).type = passed;
        twin.at(parameter).rep = repOf(program.at(passed));
        twin.parameterTypes[i] = passed;
    }
    return twin;
}

static Function createSignatureAdapter(const Program& program, const Function& like, const ValueEntrySignature& signature)
{
    Function adapter;
    adapter.name = program.emptyString;
    adapter.kind = FunctionKind::Plain;
    adapter.module = like.module;
    adapter.position = like.position;
    adapter.flags = Format::FunctionIsTyped | Format::FunctionIsHiddenInStackTraces;
    adapter.isValueEntry = true;
    adapter.resultType = signature.result;
    adapter.returnsBoxedValue = repOf(program.at(signature.result)) == Rep::Int32;
    adapter.blocks.append(Block { });
    auto append = [&](Value&& value) {
        ValueID result(adapter.values.size());
        adapter.values.append(WTF::move(value));
        adapter.blocks[0].values.append(result);
        return result;
    };
    ValueID callee = addEntryParameter(program, adapter, program.types.anything);
    ValueID thisValue = addEntryParameter(program, adapter, program.types.anything);
    Arguments given;
    for (TypeID type : signature.parameters)
        given.append(addEntryParameter(program, adapter, type));
    Value call = makeEntryValue(program, Opcode::Call, signature.result, like.position);
    call.children[0] = callee;
    call.children[1] = thisValue;
    call.arguments = WTF::move(given);
    adapter.blocks[0].terminator.kind = TerminatorKind::Return;
    adapter.blocks[0].terminator.value = append(WTF::move(call));
    return adapter;
}

static uint32_t hashOfValueCallShape(std::span<const TypeID> parameters, TypeID result)
{
    Hasher hasher;
    add(hasher, static_cast<unsigned>(parameters.size()), result);
    for (TypeID type : parameters)
        add(hasher, type);
    return hasher.hash() >> 1;
}

static bool hasNoBody(const Function& function)
{
    return function.blocks.isEmpty() || (function.blocks[0].values.isEmpty() && function.blocks[0].terminator.kind == TerminatorKind::Unreachable);
}

static bool canCallBySignature(const Program& program, const Value& call)
{
    if (!call.hasCallSignature())
        return false;
    auto& declared = program.callSignatures[call.callSignature()];
    unsigned numberOfArguments = call.arguments.size();
    if (declared.hasRestParameter || numberOfArguments < declared.minArgumentCount || numberOfArguments > declared.parameters.size())
        return false;
    unsigned numberOfGPRs = 0;
    unsigned numberOfFPRs = 0;
    for (unsigned k = 0; k < numberOfArguments; ++k) {
        if (!declared.parameters[k])
            return false;
        Rep rep = repOf(program.at(declared.parameters[k]));
        ++(rep == Rep::Double || rep == Rep::OptionalDouble ? numberOfFPRs : numberOfGPRs);
    }
    return numberOfGPRs <= numberOfArgumentGPRs && numberOfFPRs <= FPRInfo::numberOfArgumentRegisters;
}

static bool isExpensiveToBox(const Type& type)
{
    Rep rep = repOf(type);
    return rep == Rep::Double || rep == Rep::OptionalDouble || rep == Rep::Int64 || rep == Rep::OptionalInt64;
}

static bool isPassedUnboxed(const Program& program, TypeID type) { return isExpensiveToBox(program.at(type)); }

static Vector<TypeID, 4> valueCallShapeParameters(const Program& program, const Value& call)
{
    Vector<TypeID, 4> parameters;
    parameters.append(program.callSignatures[call.callSignature()].parameters.span().first(call.arguments.size()));
    for (TypeID& type : parameters) {
        if (auto widened = program.numberOrUndefinedTypes.find(type.index()); widened != program.numberOrUndefinedTypes.end())
            type = widened->value;
    }
    return parameters;
}

static constexpr uint32_t bytesSavedPerCallByResult = 40;
static constexpr uint32_t resultAdapterBytes = 190;
static constexpr uint32_t minCallsPerAdapter = resultAdapterBytes / bytesSavedPerCallByResult + 1;
static constexpr unsigned maxRequestedResults = 64;

static Vector<uint32_t> useCountsIn(const Function& function)
{
    Vector<uint32_t> result(FillWith { }, function.values.size(), 0u);
    auto count = [&](ValueID child) {
        if (child)
            ++result[child.index()];
    };
    for (auto& block : function.blocks) {
        count(block.terminator.value);
        for (auto& target : block.terminator.targets) {
            for (ValueID argument : target.arguments)
                count(argument);
        }
        for (ValueID id : block.values)
            function.at(id).forEachChild(count);
    }
    return result;
}

static ValueID castAfterCall(const Function& function, const Block& block, unsigned index, const Vector<uint32_t>& useCounts)
{
    ValueID id = block.values[index];
    if (function.at(id).isInTailPosition || index + 1 >= block.values.size() || useCounts[id.index()] != 1)
        return { };
    auto& cast = function.at(block.values[index + 1]);
    if (cast.opcode != Opcode::Cast || cast.child(0) != id || cast.immediates[0] != Format::none || cast.assertedType() || cast.parameterCheckCallee())
        return { };
    if (cast.immediates[1] != static_cast<uint32_t>(CastReason::As) && cast.immediates[1] != static_cast<uint32_t>(CastReason::Store))
        return { };
    return block.values[index + 1];
}

static FunctionID resultAdapterForCall(const Program& program, const Function& function, const Value& call, ValueID cast)
{
    static_assert(numberOfArgumentGPRs < 256);
    TypeID result = cast ? function.at(cast).type : call.type;
    if (call.opcode != Opcode::Call || function.isValueEntry || !function.has(Format::FunctionIsTyped) || !result || call.arguments.size() > numberOfArgumentGPRs)
        return { };
    auto& type = program.at(result);
    if (type.isAnything() || type.isNever() || type.hasOnlyPrimitives(Primitive::Undefined) || isExpensiveToBox(type))
        return { };
    return program.resultAdapters.get(Program::resultAdapterKey(result, cast ? function.at(cast).immediates[1] : Format::none, call.arguments.size()));
}

} // namespace

FunctionID Program::adapterForCall(const Value& call) const
{
    if (valueCallShapes.isEmpty() || call.opcode != Opcode::Call || !canCallBySignature(*this, call))
        return { };
    auto parameters = valueCallShapeParameters(*this, call);
    if (auto found = valueCallShapesByHash.find(hashOfValueCallShape(parameters.span(), call.type)); found != valueCallShapesByHash.end()) {
        for (uint32_t shape : found->value) {
            if (valueCallShapes[shape].result == call.type && std::ranges::equal(valueCallShapes[shape].parameters, parameters))
                return valueCallShapes[shape].adapter;
        }
    }
    return { };
}

void createResultAdapters(Program& program)
{
    if (program.mayRunInMultipleInstances || program.logsTypeErrors || Options::auditAOTTypedAssertions())
        return;
    auto& constProgram = std::as_const(program);
    unsigned count = constProgram.functions.size();
    struct Result {
        TypeID type;
        uint32_t resultCast { Format::none };
        uint32_t like { 0 };
        uint32_t numberOfCalls { 0 };
    };
    struct Asked {
        TypeID type;
        uint32_t resultCast;
        uint32_t numberOfArguments;
        uint32_t like;
        uint32_t numberOfCalls;
    };
    Vector<Asked> allRequestedResults;
    UncheckedKeyHashMap<uint64_t, uint32_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> requestedResultIndex;
    struct RequestedCallResult {
        TypeID type;
        uint32_t resultCast;
        uint32_t numberOfArguments;
    };
    Vector<Vector<RequestedCallResult>> requestedResultsIn(count);
    forEachIndexInParallel(count, [&](unsigned i) {
        static constexpr OpcodeSet calls { Opcode::Call };
        if (!constProgram.functions.mayContain(i, calls))
            return;
        auto& function = constProgram.functions[i];
        if (function.blocks.isEmpty() || function.isValueEntry || !function.has(Format::FunctionIsTyped))
            return;
        Vector<uint32_t> useCounts = useCountsIn(function);
        for (auto& block : function.blocks) {
            for (unsigned indexInBlock = 0; indexInBlock < block.values.size(); ++indexInBlock) {
                auto& call = function.at(block.values[indexInBlock]);
                if (!calls.contains(call.opcode) || call.arguments.size() > numberOfArgumentGPRs || constProgram.adapterForCall(call))
                    continue;
                ValueID cast = castAfterCall(function, block, indexInBlock, useCounts);
                TypeID type = cast ? function.at(cast).type : call.type;
                uint32_t resultCast = cast ? function.at(cast).immediates[1] : Format::none;
                if (!type || constProgram.at(type).isAnything() || constProgram.at(type).isNever() || constProgram.at(type).hasOnlyPrimitives(Primitive::Undefined) || isExpensiveToBox(constProgram.at(type)))
                    continue;
                requestedResultsIn[i].append(RequestedCallResult { type, resultCast, static_cast<uint32_t>(call.arguments.size()) });
            }
        }
    });
    for (unsigned i = 0; i < count; ++i) {
        for (auto [type, resultCast, numberOfArguments] : requestedResultsIn[i]) {
            auto added = requestedResultIndex.add(Program::resultAdapterKey(type, resultCast, numberOfArguments), static_cast<uint32_t>(allRequestedResults.size()));
            if (added.isNewEntry)
                allRequestedResults.append(Asked { type, resultCast, numberOfArguments, i, 0 });
            ++allRequestedResults[added.iterator->value].numberOfCalls;
        }
    }
    Vector<Result> results;
    UncheckedKeyHashMap<uint64_t, uint32_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> resultIndex;
    Vector<std::pair<uint32_t, uint32_t>> adaptersToCreate;
    for (auto& asked : allRequestedResults) {
        if (asked.numberOfCalls < minCallsPerAdapter)
            continue;
        auto added = resultIndex.add(Program::resultAdapterKey(asked.type, asked.resultCast, 0), static_cast<uint32_t>(results.size()));
        if (added.isNewEntry)
            results.append(Result { asked.type, asked.resultCast, asked.like, 0 });
        results[added.iterator->value].numberOfCalls += asked.numberOfCalls;
        adaptersToCreate.append({ added.iterator->value, asked.numberOfArguments });
    }
    if (results.isEmpty())
        return;

    struct Candidate {
        FunctionID general;
        TypeID resultType;
    };
    Vector<TypeID> resultTypes;
    UncheckedKeyHashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> resultTypeIndex;
    auto candidates = collectInParallel<Candidate>(count, [&](unsigned index, Vector<Candidate>& collected) {
        auto entrypoint = constProgram.genericEntrypoints.find(index);
        if (entrypoint == constProgram.genericEntrypoints.end())
            return;
        FunctionID id(index);
        FunctionID general = entrypoint->value;
        auto& function = constProgram.at(id);
        if (!isPlainFunctionWithKnownCallers(constProgram, id) || function.has(Format::FunctionIsBuiltin) || !function.has(Format::FunctionIsTyped) || !function.resultType || !constProgram.usesSlimClosures(function) || constProgram.at(general).valueEntry)
            return;
        collected.append({ general, function.resultType });
    });
    for (auto [general, resultType] : candidates) {
        if (resultTypeIndex.add(resultType.index(), static_cast<uint32_t>(resultTypes.size())).isNewEntry)
            resultTypes.append(resultType);
    }
    Vector<uint32_t> mostRequestedFirst(results.size(), [](size_t index) {
        return static_cast<uint32_t>(index);
    });
    std::ranges::sort(mostRequestedFirst, [&](uint32_t a, uint32_t b) {
        return results[a].numberOfCalls != results[b].numberOfCalls ? results[a].numberOfCalls > results[b].numberOfCalls : a < b;
    });
    mostRequestedFirst.shrink(std::min<size_t>(mostRequestedFirst.size(), maxRequestedResults));
    Vector<uint64_t> compatibleResults(FillWith { }, resultTypes.size(), static_cast<uint64_t>(0));
    forEachIndexInParallel(resultTypes.size(), [&](unsigned k) {
        auto& returnedValue = constProgram.at(resultTypes[k]);
        if (returnedValue.isNever())
            return;
        for (unsigned bit = 0; bit < mostRequestedFirst.size(); ++bit) {
            auto& result = results[mostRequestedFirst[bit]];
            if (result.resultCast == Format::none ? returnedValue.isSubsetOf(constProgram.at(result.type), constProgram) : constProgram.castIsRedundant(returnedValue, constProgram.at(result.type), static_cast<CastReason>(result.resultCast)))
                compatibleResults[k] |= 1ULL << bit;
        }
    });
    uint64_t usedCompatibleResults = 0;
    uint64_t numberOfFunctions = 0;
    for (auto [general, resultType] : candidates) {
        uint64_t holding = compatibleResults[resultTypeIndex.get(resultType.index())];
        if (!holding)
            continue;
        program.functions[general.index()].compatibleResultsMask = holding;
        program.functions[general.index()].resultTypeAtAdapterSelection = resultType;
        usedCompatibleResults |= holding;
        ++numberOfFunctions;
    }
    Vector<uint32_t> resultBit(FillWith { }, results.size(), static_cast<uint32_t>(Format::none));
    for (unsigned bit = 0; bit < mostRequestedFirst.size(); ++bit) {
        if (usedCompatibleResults >> bit & 1)
            resultBit[mostRequestedFirst[bit]] = bit;
    }
    Vector<Function> created;
    for (auto [result, numberOfArguments] : adaptersToCreate) {
        if (resultBit[result] == Format::none)
            continue;
        auto& like = constProgram.functions[results[result].like];
        uint32_t resultCast = results[result].resultCast;
        ValueEntrySignature signature;
        signature.parameters.fill(constProgram.types.anything, numberOfArguments);
        signature.result = resultCast == Format::none ? results[result].type : constProgram.types.anything;
        program.resultAdapters.set(Program::resultAdapterKey(results[result].type, resultCast, numberOfArguments), FunctionID(count + created.size()));
        created.append(createSignatureAdapter(constProgram, like, signature));
        auto& adapter = created.last();
        if (resultCast != Format::none) {
            Value cast = makeEntryValue(constProgram, Opcode::Cast, results[result].type, like.position);
            cast.children[0] = adapter.blocks[0].terminator.value;
            cast.immediates[1] = resultCast;
            adapter.blocks[0].terminator.value = ValueID(adapter.values.size());
            adapter.blocks[0].values.append(adapter.blocks[0].terminator.value);
            adapter.values.append(WTF::move(cast));
            adapter.resultType = results[result].type;
        }
        adapter.returnsBoxedValue = true;
        adapter.adapterResult = resultBit[result];
    }
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT: indirect calls by result type: ", results.size(), " result types are requested by ", minCallsPerAdapter, " or more calls in the unoptimized program; the ", mostRequestedFirst.size(), " most requested are considered: ", created.size(), " adapters; ", numberOfFunctions, " functions of ", candidates.size(), ", which return ", resultTypes.size(), " types, return a subtype of one of them");
    program.functions.appendVector(WTF::move(created));
}

void clearCallerCheckedResultTypes(Program& program)
{
    auto& constProgram = std::as_const(program);
    forEachIndexInParallel(constProgram.functions.size(), [&](unsigned index) {
        auto entrypoint = constProgram.genericEntrypoints.find(index);
        if (entrypoint == constProgram.genericEntrypoints.end())
            return;
        FunctionID general = entrypoint->value;
        if (constProgram.at(general).compatibleResultsMask && (constProgram.functions[index].resultType != constProgram.at(general).resultTypeAtAdapterSelection || constProgram.functions[index].isCalledFromDroppableFrame))
            program.functions[general.index()].compatibleResultsMask = 0;
    });
}

void createValueEntrypoints(Program& program)
{
    if (program.mayRunInMultipleInstances || program.logsTypeErrors || Options::auditAOTTypedAssertions() || program.callSignatures.isEmpty())
        return;
    auto& constProgram = std::as_const(program);
    unsigned count = constProgram.functions.size();
    struct Shape {
        ValueEntrySignature signature;
        uint32_t like { 0 };
        uint32_t numberOfSites { 0 };
        FunctionID adapter;
    };
    Vector<Shape> shapes;
    UncheckedKeyHashMap<uint32_t, Vector<uint32_t, 1>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> shapesByHash;
    Vector<TypeID> typesOrUndefined;
    auto orUndefined = [&](TypeID id) {
        if (typesOrUndefined.size() <= id.index())
            typesOrUndefined.grow(id.index() + 1);
        if (!typesOrUndefined[id.index()]) {
            Type type = constProgram.at(id);
            type.primitives |= Primitive::Undefined;
            typesOrUndefined[id.index()] = program.types.intern(type);
        }
        return typesOrUndefined[id.index()];
    };
    for (auto& signature : constProgram.callSignatures) {
        for (TypeID type : signature.parameters) {
            if (type && constProgram.at(type).isNumber())
                program.numberOrUndefinedTypes.add(type.index(), orUndefined(type));
        }
    }
    struct Site {
        Vector<TypeID, 4> parameters;
        TypeID result;
    };
    Vector<Vector<Site>> sitesIn(count);
    forEachIndexInParallel(count, [&](unsigned i) {
        static constexpr OpcodeSet callsWithSignature { Opcode::Call, Opcode::CallMethod };
        if (!constProgram.functions.mayContain(i, callsWithSignature))
            return;
        auto& function = constProgram.functions[i];
        if (function.blocks.isEmpty() || !function.has(Format::FunctionIsTyped))
            return;
        constProgram.functions.forEachValue(i, callsWithSignature, [&](ValueID id) {
            auto& call = function.at(id);
            if (!canCallBySignature(constProgram, call))
                return;
            auto parameters = valueCallShapeParameters(constProgram, call);
            if (!isPassedUnboxed(constProgram, call.type) && std::ranges::none_of(parameters, [&](TypeID type) { return isPassedUnboxed(constProgram, type); }))
                return;
            if (constProgram.at(call.type).hasCells() || std::ranges::any_of(parameters, [&](TypeID type) { return constProgram.at(type).hasCells(); }))
                return;
            sitesIn[i].append(Site { WTF::move(parameters), call.type });
        });
    });
    for (unsigned i = 0; i < count; ++i) {
        for (auto& [parameters, result] : sitesIn[i]) {
            auto& alike = shapesByHash.add(hashOfValueCallShape(parameters.span(), result), Vector<uint32_t, 1>()).iterator->value;
            uint32_t shape = std::numeric_limits<uint32_t>::max();
            for (uint32_t known : alike) {
                if (shapes[known].signature.result == result && std::ranges::equal(shapes[known].signature.parameters, parameters))
                    shape = known;
            }
            if (shape == std::numeric_limits<uint32_t>::max()) {
                shape = shapes.size();
                alike.append(shape);
                ValueEntrySignature signature;
                signature.parameters.appendVector(parameters);
                signature.result = result;
                shapes.append(Shape { WTF::move(signature), i, 0, { } });
            }
            ++shapes[shape].numberOfSites;
        }
    }
    if (shapes.isEmpty())
        return;

    struct Answer {
        FunctionID function;
        uint32_t shape { Format::none };
        unsigned argumentCount { 0 };
    };
    auto candidates = collectInParallel<FunctionID>(constProgram.functions.size(), [&](unsigned index, Vector<FunctionID>& collected) {
        if (constProgram.genericEntrypoints.contains(index) && canHaveValueEntry(constProgram, FunctionID(index)))
            collected.append(FunctionID(index));
    });
    Vector<Vector<TypeID, 4>> possibleParameterTypes(candidates.size());
    for (unsigned k = 0; k < candidates.size(); ++k) {
        for (TypeID type : constProgram.at(candidates[k]).parameterTypes)
            possibleParameterTypes[k].append(!type ? constProgram.types.anything : constProgram.at(type).isNumber() ? orUndefined(type) : type);
    }
    Vector<uint32_t> shapesBySiteCount(shapes.size(), [](size_t index) {
        return static_cast<uint32_t>(index);
    });
    std::ranges::sort(shapesBySiteCount, [&](uint32_t a, uint32_t b) {
        return shapes[a].numberOfSites != shapes[b].numberOfSites ? shapes[a].numberOfSites > shapes[b].numberOfSites : a < b;
    });
    Vector<Answer> candidateMatches(candidates.size());
    forEachIndexInParallel(candidates.size(), [&](unsigned k) {
        auto& function = constProgram.at(candidates[k]);
        auto& own = possibleParameterTypes[k];
        unsigned minArguments = own.size();
        while (minArguments && constProgram.at(constProgram.types.undefined).isSubsetOf(constProgram.at(function.parameterTypes[minArguments - 1] ? function.parameterTypes[minArguments - 1] : constProgram.types.anything), constProgram))
            --minArguments;
        auto& returnedValue = constProgram.at(function.resultType ? function.resultType : constProgram.types.anything);
        auto& answer = candidateMatches[k];
        answer.function = candidates[k];
        for (unsigned round = 0; round < 2 * shapesBySiteCount.size(); ++round) {
            uint32_t shape = shapesBySiteCount[round % shapesBySiteCount.size()];
            auto& signature = shapes[shape].signature;
            unsigned given = std::min<unsigned>(signature.parameters.size(), own.size());
            if (given < minArguments || !returnedValue.isSubsetOf(constProgram.at(signature.result), constProgram))
                continue;
            if (round < shapesBySiteCount.size() && repOf(constProgram.at(signature.result)) != repOf(returnedValue))
                continue;
            bool fits = true;
            for (unsigned i = 0; i < given && fits; ++i)
                fits = !constProgram.at(own[i]).hasCells() && (signature.parameters[i] == own[i] || constProgram.at(signature.parameters[i]).isSubsetOf(constProgram.at(own[i]), constProgram));
            if (!fits)
                continue;
            bool crosses = isPassedUnboxed(constProgram, signature.result) && isExpensiveToBox(returnedValue);
            for (unsigned i = 0; i < given && !crosses; ++i)
                crosses = isPassedUnboxed(constProgram, signature.parameters[i]);
            if (!crosses)
                continue;
            answer.shape = shape;
            answer.argumentCount = given;
            break;
        }
    });
    Vector<Answer> answers;
    for (auto& answer : candidateMatches) {
        if (answer.shape != Format::none)
            answers.append(answer);
    }
    if (answers.isEmpty())
        return;
    BitVector matchesSomeShape(count);
    for (auto& answer : answers)
        matchesSomeShape.set(answer.function.index());
    Vector<uint8_t> escapesToProgram(FillWith { }, count, static_cast<uint8_t>(0));
    for (auto& layout : constProgram.layouts) {
        for (auto* methods : { &layout.methods, &layout.staticMethods }) {
            for (auto& method : *methods) {
                if (method.function)
                    escapesToProgram[method.function.index()] = 1;
            }
        }
    }
    forEachIndexInParallel(count, [&](unsigned i) {
        auto& function = constProgram.functions[i];
        Vector<ValueID, 8> closures;
        static constexpr OpcodeSet functionValues { Opcode::FunctionReference, Opcode::NewClosure, Opcode::NewClosureOnce };
        constProgram.functions.forEachIndexedValue(i, functionValues, [&](ValueID id) {
            if (matchesSomeShape.get(function.at(id).function().index()))
                closures.append(id);
        });
        if (closures.isEmpty())
            return;
        auto recordEscapingClosure = [&](ValueID used) {
            if (closures.contains(used))
                WTF::atomicStore(&escapesToProgram[function.at(used).function().index()], static_cast<uint8_t>(1), std::memory_order_relaxed);
        };
        for (auto& block : function.blocks) {
            for (ValueID id : block.values) {
                auto& value = function.at(id);
                if (value.opcode == Opcode::Call) {
                    auto& callee = function.at(value.child(0));
                    if (callee.opcode == Opcode::GetGlobal)
                        continue;
                    if (callee.opcode == Opcode::GetById && value.child(1) && callee.child(0) == value.child(1)) {
                        auto& receiver = constProgram.typeOf(function, value.child(1));
                        if (!receiver.anyObject && !receiver.anyFunction && receiver.objectLayouts.isEmpty())
                            continue;
                    }
                    if (value.child(1))
                        recordEscapingClosure(value.child(1));
                    for (ValueID argument : value.arguments)
                        recordEscapingClosure(argument);
                    continue;
                }
                if (value.opcode == Opcode::CallMethod) {
                    auto& receiver = constProgram.typeOf(function, value.child(0));
                    if (!receiver.anyObject && !receiver.anyFunction && receiver.objectLayouts.isEmpty())
                        continue;
                }
                value.forEachChild(recordEscapingClosure);
            }
            if (block.terminator.value)
                recordEscapingClosure(block.terminator.value);
            for (auto& target : block.terminator.targets) {
                for (ValueID argument : target.arguments)
                    recordEscapingClosure(argument);
            }
        }
    });
    answers.removeAllMatching([&](auto& answer) {
        return !escapesToProgram[answer.function.index()];
    });
    if (answers.isEmpty())
        return;

    Vector<Function> created;
    for (auto& answer : answers) {
        if (shapes[answer.shape].adapter)
            continue;
        shapes[answer.shape].adapter = FunctionID(count + created.size());
        created.append(createSignatureAdapter(constProgram, constProgram.functions[shapes[answer.shape].like], shapes[answer.shape].signature));
    }
    size_t numberOfAdapters = created.size();
    program.firstSignatureAdapter = FunctionID(count);
    created.grow(numberOfAdapters + answers.size());
    forEachIndexInParallel(answers.size(), [&](unsigned k) {
        auto& answer = answers[k];
        auto& shape = shapes[answer.shape];
        ValueEntrySignature signature;
        signature.parameters.append(shape.signature.parameters.span().first(answer.argumentCount));
        signature.result = shape.signature.result;
        auto& entry = created[numberOfAdapters + k];
        entry = canHaveValueEntryClone(constProgram, answer.function, signature) ? createValueEntryClone(constProgram, answer.function, signature) : createValueEntry(constProgram, answer.function, signature);
        entry.valueEntryAdapter = shape.adapter;
    });
    for (unsigned k = 0; k < answers.size(); ++k)
        program.functions[constProgram.genericEntrypoints.get(answers[k].function.index()).index()].valueEntry = FunctionID(count + numberOfAdapters + k);
    for (auto& shape : shapes) {
        if (!shape.adapter)
            continue;
        program.valueCallShapesByHash.add(hashOfValueCallShape(shape.signature.parameters.span(), shape.signature.result), Vector<uint32_t, 1>()).iterator->value.append(program.valueCallShapes.size());
        program.valueCallShapes.append({ shape.signature.parameters, shape.signature.result, shape.adapter });
    }
    program.functions.appendVector(WTF::move(created));
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT: indirect calls by signature: ", shapes.size(), " signatures pass a double in the unoptimized program; ", answers.size(), " functions of ", candidates.size(), " implement ", numberOfAdapters, " of them");
}

void resolveCallsBySignature(Program& program)
{
    if (program.valueCallShapes.isEmpty() && program.resultAdapters.isEmpty())
        return;
    auto& constProgram = std::as_const(program);
    uint64_t numberOfSites = 0;
    uint64_t unchangedCount = 0;
    uint64_t cheapToBoxCount = 0;
    struct CallByResult {
        uint32_t function;
        ValueID call;
        ValueID cast;
        FunctionID adapter;
    };
    Vector<CallByResult> callsByResult;
    UncheckedKeyHashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> adapterCallCount;
    enum class Verdict : uint8_t { IsUnchanged, CallsByResult, IsCheapToBox, CallsBySignature };
    struct Judged {
        ValueID call;
        ValueID cast;
        FunctionID adapter;
        Verdict verdict;
    };
    Vector<Vector<Judged>> verdictsIn(constProgram.functions.size());
    forEachIndexInParallel(constProgram.functions.size(), [&](unsigned i) {
        static constexpr OpcodeSet calls { Opcode::Call };
        if (!constProgram.functions.mayContain(i, calls))
            return;
        auto& function = constProgram.functions[i];
        if (function.blocks.isEmpty() || function.isValueEntry)
            return;
        Vector<uint32_t> useCounts;
        if (!constProgram.resultAdapters.isEmpty())
            useCounts = useCountsIn(function);
        for (auto& block : function.blocks) {
            for (unsigned indexInBlock = 0; indexInBlock < block.values.size(); ++indexInBlock) {
                ValueID id = block.values[indexInBlock];
                auto& call = function.at(id);
                if (!calls.contains(call.opcode))
                    continue;
                FunctionID adapter = constProgram.adapterForCall(call);
                ValueID cast;
                if (!adapter && !constProgram.resultAdapters.isEmpty()) {
                    cast = castAfterCall(function, block, indexInBlock, useCounts);
                    adapter = resultAdapterForCall(constProgram, function, call, cast);
                }
                if (!adapter || hasNoBody(constProgram.at(adapter)))
                    continue;
                ValueID origin = call.child(0);
                while (function.at(origin).opcode == Opcode::Cast)
                    origin = function.at(origin).child(0);
                bool goes = !call.assertedType() && call.speculatedMethod == SpeculatedMethod::None && !function.at(call.child(0)).predictedClosure() && !function.at(origin).predictedClosure();
                if (LayoutID supertype = call.child(1) ? constProgram.typeOf(function, call.child(1)).singleLayout() : LayoutID(); supertype && constProgram.at(supertype).mayBeStackAllocated)
                    goes = false;
                auto& parameters = constProgram.at(adapter).blocks[0].parameters;
                for (unsigned k = 0; k < call.arguments.size() && goes; ++k) {
                    auto& given = constProgram.typeOf(function, call.arguments[k]);
                    LayoutID supertype = given.singleLayout();
                    goes = given.isSubsetOf(constProgram.typeOf(constProgram.at(adapter), parameters[k + 2]), constProgram) && !(supertype && constProgram.at(supertype).mayBeStackAllocated);
                }
                if (!goes) {
                    verdictsIn[i].append(Judged { id, cast, adapter, Verdict::IsUnchanged });
                    continue;
                }
                if (constProgram.at(adapter).adapterResult != Format::none) {
                    verdictsIn[i].append(Judged { id, cast, adapter, Verdict::CallsByResult });
                    continue;
                }
                bool passesExpensiveToBoxValue = isPassedUnboxed(constProgram, call.type);
                for (unsigned k = 0; k < call.arguments.size() && !passesExpensiveToBoxValue; ++k)
                    passesExpensiveToBoxValue = isExpensiveToBox(constProgram.typeOf(function, call.arguments[k]));
                verdictsIn[i].append(Judged { id, cast, adapter, passesExpensiveToBoxValue ? Verdict::CallsBySignature : Verdict::IsCheapToBox });
            }
        }
    });
    for (unsigned i = 0; i < constProgram.functions.size(); ++i) {
        for (auto [id, cast, adapter, verdict] : verdictsIn[i]) {
            switch (verdict) {
            case Verdict::IsUnchanged:
                ++unchangedCount;
                break;
            case Verdict::CallsByResult:
                callsByResult.append({ i, id, cast, adapter });
                ++adapterCallCount.add(adapter.index(), 0).iterator->value;
                break;
            case Verdict::IsCheapToBox:
                ++cheapToBoxCount;
                break;
            case Verdict::CallsBySignature: {
                auto& changed = program.functions.atKeepingValueIndex(i).at(id);
                changed.isSignatureCall = true;
                changed.immediates[1] = adapter.index();
                program.adaptersCalledBySignature.set(adapter.index());
                ++numberOfSites;
                break;
            }
            }
        }
    }
    uint64_t callByResultSiteCount = 0;
    uint64_t savedCastCount = 0;
    uint64_t resultAdapterCount = 0;
    uint64_t profitableResults = 0;
    int64_t bytesSavedByUpperHalf = 0;
    for (auto& [adapter, numberOfCalls] : adapterCallCount) {
        if (numberOfCalls < minCallsPerAdapter)
            continue;
        uint32_t result = constProgram.functions[adapter].adapterResult;
        profitableResults |= 1ULL << result;
        if (result >= 32)
            bytesSavedByUpperHalf += static_cast<int64_t>(numberOfCalls) * bytesSavedPerCallByResult - resultAdapterBytes;
    }
    int64_t informativeEntryCount = 0;
    for (auto& [index, general] : constProgram.genericEntrypoints)
        informativeEntryCount += !!(constProgram.at(general).compatibleResultsMask & profitableResults);
    program.compatibleResultsMaskSize = sizeof(uint64_t);
    if (bytesSavedByUpperHalf <= informativeEntryCount * static_cast<int64_t>(sizeof(uint32_t))) {
        program.compatibleResultsMaskSize = sizeof(uint32_t);
        profitableResults &= std::numeric_limits<uint32_t>::max();
    }
    for (auto& [function, id, cast, adapter] : callsByResult) {
        if (adapterCallCount.get(adapter.index()) < minCallsPerAdapter || !(profitableResults >> constProgram.at(adapter).adapterResult & 1))
            continue;
        auto& changed = program.functions.atKeepingValueIndex(function).at(id);
        changed.isSignatureCall = true;
        changed.immediates[1] = adapter.index();
        if (cast) {
            changed.type = constProgram.functions[function].at(cast).type;
            changed.rep = repOf(constProgram.at(changed.type));
            program.functions.atKeepingValueIndex(function).at(cast).setCastReason(CastReason::Upcast);
            ++savedCastCount;
        }
        ++callByResultSiteCount;
        if (program.adaptersCalledBySignature.get(adapter.index()))
            continue;
        program.adaptersCalledBySignature.set(adapter.index());
        ++resultAdapterCount;
        program.resultsUsedByCalls |= 1ULL << constProgram.at(adapter).adapterResult;
    }
    program.didResolveCallsBySignature = true;
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT: indirect calls by signature: ", numberOfSites, " calls use one; ", unchangedCount, " could, but an argument is unproven or a callee is predicted; ", cheapToBoxCount, " pass nothing that is expensive to box");
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT: indirect calls by result type: ", callByResultSiteCount, " calls of ", callsByResult.size(), " use one, ", savedCastCount, " of them including the following cast, through ", resultAdapterCount, " adapters, for ", std::popcount(program.resultsUsedByCalls), " result types; an adapter with fewer than ", minCallsPerAdapter, " calls is not worth its size; ", program.compatibleResultsMaskSize, " bytes precede generic entrypoints that describe their result");
}

void findDeclaredSuperclasses(Program& program)
{
    if (!program.usesOperation(Operations::MakeClass))
        return;
    Vector<Vector<std::pair<LayoutID, FunctionID>>> found(program.functions.size());
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        auto& function = std::as_const(program).functions[i];
        auto withoutCasts = [&](ValueID id) {
            while (function.at(id).opcode == Opcode::Cast)
                id = function.at(id).child(0);
            return id;
        };
        auto constantClosure = [&](ValueID id) {
            id = withoutCasts(id);
            return function.at(id).opcode == Opcode::FunctionReference ? function.at(id).function() : FunctionID();
        };
        static constexpr OpcodeSet operations { Opcode::Operation };
        std::as_const(program).functions.forEachValue(i, operations, [&](ValueID id) {
            auto& value = function.at(id);
            if (program.operationOf(value) != Operations::MakeClass || value.arguments.size() != 2)
                return;
            FunctionID constructor = constantClosure(value.arguments[0]);
            FunctionID parent = constantClosure(value.arguments[1]);
            if (constructor && std::as_const(program).at(constructor).constructedClass && function.at(withoutCasts(value.arguments[1])).opcode == Opcode::GetGlobal)
                found[i].append({ std::as_const(program).at(constructor).constructedClass, FunctionID() });
            if (!constructor || !parent || std::as_const(program).at(parent).kind != FunctionKind::Constructor)
                return;
            if (LayoutID ofClass = std::as_const(program).at(constructor).constructedClass; ofClass && program.at(ofClass).isEvaluatedOnce && !program.at(ofClass).base)
                found[i].append({ ofClass, parent });
        });
    });
    for (auto& perFunction : found) {
        for (auto [ofClass, parent] : perFunction) {
            if (parent)
                program.at(ofClass).declaredSuperclassConstructor = parent;
            else
                program.at(ofClass).extendsGlobalClass = true;
        }
    }
}

void createGenericEntrypoints(Program& program)
{
    for (unsigned i = 0; i < program.layouts.size(); ++i) {
        if (!program.layouts[i].type)
            program.layouts[i].type = program.types.intern(Type::forLayout(LayoutID(i)));
    }
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        static constexpr OpcodeSet closures { Opcode::NewClosure, Opcode::NewClosureOnce };
        std::as_const(program).functions.forEachValue(i, closures, [&](ValueID id) {
            WTF::atomicStore(&program.functions.atKeepingValueIndex(std::as_const(program).functions[i].at(id).function().index()).hasManyClosures, true, std::memory_order_relaxed);
        });
    });
    for (auto& layout : program.layouts) {
        bool mayBecomeRecord = layout.kind == LayoutKind::Instance && layout.specializations.containsIf([&](LayoutID specialization) {
            return program.at(specialization).kind != LayoutKind::Instance;
        });
        if (!mayBecomeRecord)
            continue;
        layout.instancesMayBecomeRecords = true;
        for (LayoutID ancestor = layout.base; ancestor; ancestor = program.at(ancestor).base)
            program.at(ancestor).instancesMayBecomeRecords = true;
    }

    BitVector isValue(program.functions.size());
    BitVector isMethodOfClass(program.functions.size());
    auto functionsUsedAsValues = findIndicesInParallel(program.functions.size(), [&](unsigned i) {
        return std::as_const(program).functions[i].has(Format::FunctionIsUsedAsValue);
    });
    for (unsigned i : functionsUsedAsValues)
        isValue.set(i);
    for (auto& layout : program.layouts) {
        if (layout.constructor)
            isValue.set(layout.constructor.index());
        for (auto* methods : { &layout.methods, &layout.staticMethods }) {
            for (auto& method : *methods) {
                isValue.set(method.function.index());
                if (layout.kind == LayoutKind::Instance)
                    isMethodOfClass.set(method.function.index());
            }
        }
    }
    auto declaredGenericEntrypoints = findIndicesInParallel(program.functions.size(), [&](unsigned i) {
        return std::as_const(program).functions[i].has(Format::FunctionIsDeclaredGenericEntrypoint);
    });
    for (unsigned i : declaredGenericEntrypoints) {
        bool isForClassMethod = false;
        for (auto& value : std::as_const(program).functions[i].values) {
            if (value.opcode != Opcode::CallDirect)
                continue;
            isForClassMethod = isMethodOfClass.get(value.function().index());
            break;
        }
        bool isCreatedForLogging = program.logsTypeErrors && !isForClassMethod;
        if (isCreatedForLogging)
            program.functions[i].flags |= FunctionLogsTypeMismatches;
        FunctionID served;
        for (auto& value : std::as_const(program).functions[i].values) {
            if (value.opcode != Opcode::CallDirect)
                continue;
            if (served) {
                program.functions[served.index()].untypedClone = value.function();
                break;
            }
            auto& entry = std::as_const(program).functions[i];
            auto& candidate = std::as_const(program).at(value.function());
            bool fits = !isCreatedForLogging || (entry.kind == candidate.kind && entry.length == candidate.length && entry.captureTypes == candidate.captureTypes && entry.parameterTypes.size() == candidate.parameterTypes.size()
                && entry.has(Format::FunctionHasThis) == candidate.has(Format::FunctionHasThis) && entry.has(Format::FunctionHasRestParameter) == candidate.has(Format::FunctionHasRestParameter) && !entry.takesCalleeClosure && !candidate.takesCalleeClosure);
            if (fits && isValue.get(value.function().index()) && !candidate.rampForClosures) {
                program.functions[value.function().index()].declaredGenericEntrypoint = FunctionID(i);
                served = value.function();
            }
            if (!served || !isCreatedForLogging)
                break;
        }
    }
    auto functionsWithLoggingEntrypoints = findIndicesInParallel(program.functions.size(), [&](unsigned i) {
        FunctionID written = std::as_const(program).functions[i].declaredGenericEntrypoint;
        return written && std::as_const(program).at(written).has(FunctionLogsTypeMismatches);
    });
    for (unsigned i : functionsWithLoggingEntrypoints) {
        FunctionID written = std::as_const(program).functions[i].declaredGenericEntrypoint;
        Function copy = std::as_const(program).at(written);
        copy.flags |= FunctionIsEnteredByValue;
        program.functions[i].declaredGenericEntrypoint = FunctionID(program.functions.size());
        program.functions.append(WTF::move(copy));
    }
    auto entered = findIndicesInParallel(isValue.size(), [&](unsigned i) {
        return isValue.get(i) && !std::as_const(program).functions[i].isGenericEntrypoint() && !program.genericEntrypoints.contains(i);
    });
    unsigned firstEntry = program.functions.size();
    program.functions.grow(firstEntry + entered.size());
    program.firstGenericEntrypoint = firstEntry;
    program.numberOfConsecutiveGenericEntrypoints = entered.size();
    forEachIndexInParallel(entered.size(), [&](unsigned k) {
        program.functions[firstEntry + k] = createGenericEntrypoint(program, FunctionID(entered[k]), std::as_const(program).functions[entered[k]].declaredGenericEntrypoint);
    });
    for (unsigned k = 0; k < entered.size(); ++k)
        program.genericEntrypoints.add(entered[k], FunctionID(firstEntry + k));
}

static Function createBoundFunction(const Program& program, FunctionID id, unsigned numberOfBoundArguments, StringID name)
{
    auto& callee = program.at(id);
    auto& types = program.types;
    unsigned taken = std::min<unsigned>(numberOfBoundArguments, callee.parameterTypes.size());
    Function bound;
    bound.name = name;
    bound.kind = FunctionKind::Bound;
    bound.module = callee.module;
    bound.position = callee.position;
    bound.length = callee.length > numberOfBoundArguments ? callee.length - numberOfBoundArguments : 0;
    bound.flags = Format::FunctionIsUsedAsValue | Format::FunctionIsHiddenInStackTraces | (callee.flags & (Format::FunctionIsTyped | Format::FunctionIsInBuiltinModule));
    bound.declaredAs = callee.declaredAs & (functionIsInBuiltinModule | functionIsAsync);
    bound.hasManyClosures = true;
    bound.resultType = callee.resultType;
    bound.blocks.append(Block { });

    Arguments all;
    auto addParameter = [&](TypeID type) {
        ValueID result(bound.values.size());
        Value value;
        value.opcode = Opcode::Parameter;
        value.position = callee.position;
        value.type = type ? type : types.anything;
        value.rep = repOf(program.at(value.type));
        value.immediates = { 0, static_cast<uint32_t>(bound.blocks[0].parameters.size()) };
        bound.values.append(WTF::move(value));
        bound.blocks[0].parameters.append(result);
        all.append(result);
        return bound.values.last().type;
    };
    for (TypeID type : callee.captureTypes)
        bound.captureTypes.append(addParameter(type));
    if (callee.has(Format::FunctionHasThis))
        bound.captureTypes.append(addParameter(callee.thisType));
    for (unsigned i = 0; i < taken; ++i)
        bound.captureTypes.append(addParameter(callee.parameterTypes[i]));
    if (!callee.captureTypes.isEmpty() || callee.hasManyClosures) {
        bound.captureTypes.append(addParameter(TypeID()));
        all.removeLast();
    }
    for (unsigned i = taken; i < callee.parameterTypes.size(); ++i) {
        addParameter(callee.parameterTypes[i]);
        bound.parameterTypes.append(callee.parameterTypes[i]);
        if (callee.annotatedParameters.get(i))
            bound.annotatedParameters.set(i - taken);
        if (callee.assertedParameterType(i)) {
            bound.assertedParameterTypes.grow(i - taken + 1);
            bound.assertedParameterTypes[i - taken] = callee.assertedParameterType(i);
        }
    }

    Value call;
    call.opcode = Opcode::CallDirect;
    call.position = callee.position;
    call.type = callee.resultType ? callee.resultType : types.anything;
    call.rep = repOf(program.at(call.type));
    call.immediates[0] = id.index();
    call.arguments = WTF::move(all);
    bound.values.append(WTF::move(call));
    bound.blocks[0].values.append(ValueID(bound.values.size() - 1));
    bound.blocks[0].terminator.kind = TerminatorKind::Return;
    bound.blocks[0].terminator.value = bound.blocks[0].values.last();
    return bound;
}

void expandDisposableStacks(Program& program)
{
    if (!program.usesOperation(Operations::NewDisposableStack))
        return;
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        static constexpr OpcodeSet operations { Opcode::Operation };
        bool createsDisposableStack = false;
        std::as_const(program).functions.forEachIndexedValue(i, operations, [&](ValueID id) {
            createsDisposableStack |= program.operationOf(std::as_const(program).functions[i].at(id)) == Operations::NewDisposableStack;
        });
        if (createsDisposableStack)
            CallResolution(program, program.functions[i]).expandDisposableStacks();
    });
}

void createBoundFunctions(Program& program)
{
    Vector<Vector<uint64_t>> boundFunctionsNeededBy(program.functions.size());
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        static constexpr OpcodeSet methodCalls { Opcode::CallMethod };
        if (!program.functions.mayContain(i, methodCalls))
            return;
        auto& function = program.functions.atKeepingValueIndex(i);
        if (function.isGenericEntrypoint())
            return;
        CallResolution resolution(program, function);
        std::as_const(program).functions.forEachValue(i, methodCalls, [&](ValueID id) {
            resolution.collectBoundFunction(id, boundFunctionsNeededBy[i]);
        });
    });
    Vector<uint64_t> wanted;
    for (auto& perFunction : boundFunctionsNeededBy)
        wanted.appendVector(perFunction);
    for (uint64_t key : wanted) {
        if (program.boundFunctions.contains(key))
            continue;
        FunctionID function(static_cast<uint32_t>(key >> 2));
        Vector<uint8_t> name;
        name.append(asBytes("bound "_s.span8()));
        name.append(program.strings[std::as_const(program).at(function).name.index()]);
        StringID boundName = program.intern(name.span());
        FunctionID bound(program.functions.size());
        program.functions.append(createBoundFunction(program, function, key & 3, boundName));
        program.boundFunctions.add(key, bound);
        Function entry = createGenericEntrypoint(program, bound);
        program.genericEntrypoints.add(bound.index(), FunctionID(program.functions.size()));
        program.functions.append(WTF::move(entry));
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
