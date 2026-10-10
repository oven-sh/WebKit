/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedPasses.h"

#if ENABLE(AOT)

#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

// Splits an async function or a generator into a ramp function and a resume function.
//
// The ramp keeps the identity and the signature of the original. That of a generator allocates the coroutine frame and stores the arguments
// into it. That of an async function creates the result promise and calls the start function: the original body up to the first Await on
// every path, where the frame is allocated. An async function that returns before it awaits anything has no frame.
//
// The resume function is the original body behind a switch on the frame's state. At each Await, the SSA values that are live across
// it are spilled to frame fields and the function returns. The matching resume block reloads them, and SSA form is repaired with
// block parameters. Between two suspension points values stay in SSA form, so B3 keeps them in registers.
//
// An async function goes on in place where nobody can tell: see InlineBuiltin::AwaitMayContinue.
//
// No type is found out. A value that is kept in the frame gives its field its own type, and the reload and the block parameters that
// stand in for it copy that. The fixed fields have the types their contract says, see CoroutineFrame. So placement picks an unboxed
// representation for a spill field exactly where the value has one.
class CoroutineSplitter {
public:
    CoroutineSplitter(Program& program, FunctionID ramp)
        : m_program(program)
        , m_ramp(ramp)
    {
    }

    void run()
    {
        m_isGenerator = m_program.at(m_ramp).has(Format::FunctionIsGenerator);
        m_isAsync = m_program.at(m_ramp).has(Format::FunctionIsAsync);
        m_startsEagerly = m_program.at(m_ramp).has(Format::FunctionStartsEagerly);
        m_position = m_program.at(m_ramp).position;
        createFrameLayout();
        if (!m_isGenerator)
            copyBodyToStartFunction();
        moveBodyToResumeFunction();
        createDispatchBlock();
        replaceResumeModeReads();
        splitAtSuspensionPoints();
        rewriteReturns();
        spillAndReload();
        finishDispatchBlock();
        m_program.functions.append(WTF::move(m_body));
        if (!m_isGenerator) {
            finishStartFunction();
            m_program.functions.append(WTF::move(m_startBody));
        }
        buildRamp();
    }

private:
    struct SuspensionPoint {
        BlockID suspendBlock;
        BlockID resumeBlock;
        // Where in the suspend block the spills go: before the store of the state.
        unsigned spillIndex { 0 };
        // The Await or Yield.
        ValueID suspension;
        bool isReachable { true };
        // Not so if only the start function gets there.
        bool isReachableInResumeFunction { true };
        // What is kept in the frame across it, each with its field.
        Vector<std::pair<ValueID, uint32_t>> spilled { };
    };

    static constexpr int64_t resumeModeNormal = 0;
    static constexpr int64_t resumeModeThrow = 1;
    static constexpr int64_t resumeModeReturn = 2;

    ValueID addValue(Function& function, Value&& value)
    {
        ValueID id(function.values.size());
        value.position = m_position;
        function.values.append(WTF::move(value));
        return id;
    }

    ValueID append(Function& function, BlockID block, Value&& value)
    {
        ValueID id = addValue(function, WTF::move(value));
        function.at(block).values.append(id);
        return id;
    }

    BlockID addBlock(Function& function)
    {
        function.blocks.append(Block { });
        return BlockID(function.blocks.size() - 1);
    }

    Value typed(Opcode opcode, TypeID type) const
    {
        Value value;
        value.opcode = opcode;
        value.type = type;
        value.rep = repOf(m_program.at(type));
        return value;
    }

    ValueID addParameter(Function& function, BlockID block, TypeID type)
    {
        Value parameter = typed(Opcode::Parameter, type);
        parameter.immediates = { block.index(), static_cast<uint32_t>(function.at(block).parameters.size()) };
        ValueID id = addValue(function, WTF::move(parameter));
        function.at(block).parameters.append(id);
        return id;
    }

    Value undefined() const { return typed(Opcode::Undefined, m_program.types.undefined); }

    Value integer(int64_t number, TypeID type) const
    {
        Value value = typed(Opcode::Integer, type);
        value.setBits(static_cast<uint64_t>(number));
        return value;
    }

    Value getField(ValueID frame, uint32_t field) const
    {
        Value value = typed(Opcode::GetField, m_program.at(m_frameLayout).fields[field].type);
        value.children[0] = frame;
        value.immediates = { m_frameLayout.index(), field };
        return value;
    }

    Value putField(ValueID frame, uint32_t field, ValueID stored) const
    {
        Value value = typed(Opcode::PutField, m_program.types.undefined);
        value.children[0] = frame;
        value.children[1] = stored;
        value.immediates = { m_frameLayout.index(), field };
        return value;
    }

    Value cast(ValueID operand, TypeID type, CastReason reason) const
    {
        Value value = typed(Opcode::Cast, type);
        value.children[0] = operand;
        value.immediates[1] = static_cast<uint32_t>(reason);
        return value;
    }

    Value operation(ASCIILiteral name, TypeID type, std::initializer_list<ValueID> arguments)
    {
        Value value = typed(Opcode::Operation, type);
        value.immediates[0] = m_program.intern(name.span8()).index();
        for (ValueID argument : arguments)
            value.arguments.append(argument);
        return value;
    }

    Value callResumeFunction(ValueID frame, ValueID mode, ValueID resumeValue) const
    {
        Value value = typed(Opcode::CallDirect, m_resultTypeOfResumeFunction);
        value.immediates[0] = m_resume.index();
        value.arguments.append(frame);
        value.arguments.append(mode);
        value.arguments.append(resumeValue);
        return value;
    }

    uint32_t addFrameField(ASCIILiteral name, TypeID type, uint32_t flags = 0)
    {
        Field field;
        field.name = m_program.intern(name.span8());
        field.type = type;
        field.flags = flags;
        auto& fields = m_program.at(m_frameLayout).fields;
        fields.append(WTF::move(field));
        return fields.size() - 1;
    }

    void createFrameLayout()
    {
        auto& types = m_program.types;
        m_frameLayout = LayoutID(m_program.layouts.size());
        m_frameType = types.intern(Type::view(m_frameLayout));
        m_resumeModeType = types.intern(Type::integer({ resumeModeNormal, m_isGenerator ? resumeModeReturn : resumeModeThrow | CoroutineFrame::isEnteredByJobLoop }));
        // What NewPromise makes is a promise. What stands for a generator is an object of the run time, which has no class of the engine.
        m_driverType = m_isGenerator ? types.object : types.intern(Type::engineClass(Builtins::singleton().promiseClass()));
        // An async function settles its promise. A generator hands what it yields and what it returns to its driver.
        m_resultTypeOfResumeFunction = m_isGenerator ? types.anything : types.undefined;

        Layout layout;
        layout.kind = LayoutKind::Environment;
        layout.name = m_program.at(m_ramp).name;
        layout.isCoroutineFrame = true;
        layout.type = m_frameType;
        m_program.layouts.append(WTF::move(layout));
        static_assert(!CoroutineFrame::stateField);
        RELEASE_ASSERT(addFrameField("state"_s, types.int32) == CoroutineFrame::stateField);
        RELEASE_ASSERT(addFrameField("promise"_s, m_driverType, fieldIsImmutable) == CoroutineFrame::promiseField);
        RELEASE_ASSERT(addFrameField("resumeClosure"_s, types.function, fieldIsImmutable) == CoroutineFrame::resumeClosureField);
    }

    // Values keep their numbers in both copies of the body.
    void copyBodyToStartFunction()
    {
        auto& ramp = m_program.at(m_ramp);
        m_startBody.name = ramp.name;
        m_startBody.module = ramp.module;
        m_startBody.position = ramp.position;
        m_startBody.kind = FunctionKind::Plain;
        m_startBody.flags = ramp.flags & Format::FunctionIsTyped;
        m_startBody.resultType = m_program.types.undefined;
        m_startBody.blocks = ramp.blocks;
        m_startBody.values = ramp.values;
    }

    void moveBodyToResumeFunction()
    {
        auto& ramp = m_program.at(m_ramp);
        m_resume = FunctionID(m_program.functions.size());
        m_body.name = ramp.name;
        m_body.module = ramp.module;
        m_body.position = ramp.position;
        m_body.kind = FunctionKind::Plain;
        // Its closure is entered at the function itself: see Lowering::lowerNewClosure(). So it has no generic entrypoint.
        m_body.flags = ramp.flags & Format::FunctionIsTyped;
        m_body.captureTypes.append(m_frameType);
        m_body.parameterTypes.append(m_resumeModeType);
        m_body.parameterTypes.append(m_program.types.anything);
        m_body.resultType = m_resultTypeOfResumeFunction;
        m_body.blocks = std::exchange(ramp.blocks, { });
        m_body.values = std::exchange(ramp.values, { });
        ramp.flags &= ~(Format::FunctionIsAsync | Format::FunctionIsGenerator | Format::FunctionStartsEagerly);
        ramp.resumeFunction = m_resume;
        if (!m_isGenerator)
            ramp.startFunction = FunctionID(m_resume.index() + 1);
    }

    // Block 0 becomes the dispatch block. The original entry block moves to the end, and its parameters become loads from the frame.
    void createDispatchBlock()
    {
        m_start = addBlock(m_body);
        m_body.at(m_start) = std::exchange(m_body.blocks[0], Block { });
        for (auto& block : m_body.blocks) {
            for (auto& target : block.terminator.targets) {
                if (!target.block.index())
                    target.block = m_start;
            }
        }
        m_frame = addParameter(m_body, BlockID(0u), m_frameType);
        m_resumeMode = addParameter(m_body, BlockID(0u), m_resumeModeType);
        m_resumeValue = addParameter(m_body, BlockID(0u), m_program.types.anything);

        Vector<ValueID> arguments = std::exchange(m_body.at(m_start).parameters, { });
        for (ValueID argument : arguments) {
            m_argumentTypes.append(m_body.at(argument).type);
            uint32_t field = addFrameField("argument"_s, m_argumentTypes.last(), fieldIsImmutable);
            Value load = getField(m_frame, field);
            load.position = m_body.at(argument).position;
            load.takePredictionOf(m_body.at(argument));
            m_body.at(argument) = WTF::move(load);
        }
        m_body.at(m_start).values.insertVector(0, arguments);
    }

    void replaceResumeModeReads()
    {
        auto replace = [&](ValueID& child) {
            if (m_body.at(child).opcode == Opcode::GetResumeMode)
                child = m_resumeMode;
        };
        for (auto& value : m_body.values)
            value.forEachChild(replace);
        for (auto& block : m_body.blocks) {
            block.terminator.forEachChild(replace);
            block.values.removeAllMatching([&](ValueID id) {
                return m_body.at(id).opcode == Opcode::GetResumeMode;
            });
        }
    }

    void splitAtSuspensionPoints()
    {
        for (unsigned b = 0; b < m_body.blocks.size(); ++b) {
            for (unsigned i = 0; i < m_body.blocks[b].values.size(); ++i) {
                Opcode opcode = m_body.at(m_body.blocks[b].values[i]).opcode;
                if (opcode != Opcode::Await && opcode != Opcode::Yield)
                    continue;
                splitAt(BlockID(b), i);
                break;
            }
        }
    }

    void splitAt(BlockID suspendBlock, unsigned index)
    {
        ValueID suspension = m_body.at(suspendBlock).values[index];
        ValueID operand = m_body.at(suspension).child(0);
        TypeID type = m_body.at(suspension).type;
        bool isYield = m_body.at(suspension).opcode == Opcode::Yield;
        m_position = m_body.at(suspension).position;

        // The rest of the block. It is visited later by the loop in splitAtSuspensionPoints.
        BlockID continuation = addBlock(m_body);
        {
            auto& from = m_body.at(suspendBlock);
            auto& to = m_body.at(continuation);
            for (unsigned i = index + 1; i < from.values.size(); ++i)
                to.values.append(from.values[i]);
            to.terminator = std::exchange(from.terminator, Terminator { });
            from.values.shrink(index);
        }
        // An Await that is watched by a handler ends its block. Whatever takes its place and may throw unwinds to the same handler.
        std::optional<Target> handler;
        if (m_body.at(continuation).terminator.kind == TerminatorKind::Invoke && m_body.at(continuation).values.isEmpty()) {
            auto& terminator = m_body.at(continuation).terminator;
            handler = terminator.targets[1];
            Target normal = terminator.targets[0];
            terminator = Terminator { };
            terminator.kind = TerminatorKind::Jump;
            terminator.targets.append(WTF::move(normal));
        }
        // The value becomes a parameter of the continuation: what the coroutine is resumed with, or what it goes on with in place. Nothing
        // vouches for either, so it is held to the type of the value.
        Value original = m_body.at(suspension);
        bool isHeld = original.assertedType || !m_program.at(type).isAnything();
        Value parameter = typed(Opcode::Parameter, type);
        parameter.immediates = { continuation.index(), 0 };
        parameter.position = m_position;
        parameter.takePredictionOf(original);
        m_body.at(suspension) = WTF::move(parameter);
        m_body.at(continuation).parameters.append(suspension);
        auto goOnWith = [&](BlockID from, ValueID yielded) {
            if (isHeld) {
                Value held = cast(yielded, type, CastReason::Claim);
                held.assertedType = original.assertedType;
                held.takePredictionOf(original);
                yielded = append(m_body, from, WTF::move(held));
            }
            auto& terminator = m_body.at(from).terminator;
            terminator.kind = isHeld && handler ? TerminatorKind::Invoke : TerminatorKind::Jump;
            terminator.targets.append(Target { continuation, { yielded } });
            if (terminator.kind == TerminatorKind::Invoke)
                terminator.targets.append(*handler);
        };
        auto branch = [&](BlockID from, ValueID condition, BlockID taken, BlockID notTaken) {
            auto& terminator = m_body.at(from).terminator;
            terminator.kind = TerminatorKind::Branch;
            terminator.value = condition;
            terminator.targets.append(Target { taken, { } });
            terminator.targets.append(Target { notTaken, { } });
        };

        BlockID resumeBlock = addBlock(m_body);
        if (isYield) {
            // The front end dispatches on the resume mode itself.
            goOnWith(resumeBlock, m_resumeValue);
        } else {
            BlockID normalBlock = addBlock(m_body);
            goOnWith(normalBlock, m_resumeValue);
            BlockID throwBlock = addBlock(m_body);
            m_body.at(throwBlock).terminator.value = m_resumeValue;
            m_body.at(throwBlock).terminator.kind = TerminatorKind::Throw;
            if (handler)
                m_body.at(throwBlock).terminator.targets.append(*handler);

            ValueID throwMode = append(m_body, resumeBlock, integer(resumeModeThrow, m_resumeModeType));
            ValueID mode = m_resumeMode;
            if (!m_isGenerator) {
                Value withoutFlags = typed(Opcode::BitAnd, m_resumeModeType);
                withoutFlags.children[0] = m_resumeMode;
                withoutFlags.children[1] = throwMode;
                mode = append(m_body, resumeBlock, WTF::move(withoutFlags));
            }
            Value isThrow = typed(Opcode::StrictEq, m_program.types.boolean);
            isThrow.children[0] = mode;
            isThrow.children[1] = throwMode;
            branch(resumeBlock, append(m_body, resumeBlock, WTF::move(isThrow)), throwBlock, normalBlock);
        }

        if (!isYield && !m_isGenerator) {
            BlockID inPlaceBlock = addBlock(m_body);
            BlockID parkBlock = addBlock(m_body);
            branch(suspendBlock, append(m_body, suspendBlock, operation("AwaitMayContinue"_s, m_program.types.boolean, { operand, m_resumeMode })), inPlaceBlock, parkBlock);
            goOnWith(inPlaceBlock, append(m_body, inPlaceBlock, operation("AwaitedValue"_s, m_program.types.anything, { operand })));
            suspendBlock = parkBlock;
        }

        m_suspensionPoints.append({ suspendBlock, resumeBlock, static_cast<unsigned>(m_body.at(suspendBlock).values.size()), suspension });
        m_body.suspensionPositions.append(m_position);
        ValueID state = append(m_body, suspendBlock, integer(m_suspensionPoints.size(), m_program.types.int32));
        append(m_body, suspendBlock, putField(m_frame, CoroutineFrame::stateField, state));
        ValueID returned = operand;
        if (!isYield) {
            // What goes wrong with what is awaited is not thrown here: the function is resumed with it.
            ValueID closure = append(m_body, suspendBlock, getField(m_frame, CoroutineFrame::resumeClosureField));
            append(m_body, suspendBlock, operation(m_isGenerator ? "Await"_s : "SuspendAsyncFunction"_s, m_program.types.undefined, { closure, operand }));
            returned = append(m_body, suspendBlock, undefined());
        }
        m_body.at(suspendBlock).terminator.kind = TerminatorKind::Return;
        m_body.at(suspendBlock).terminator.value = returned;
    }

    void rewriteReturns()
    {
        BitVector isSuspendBlock(m_body.blocks.size());
        for (auto& point : m_suspensionPoints)
            isSuspendBlock.set(point.suspendBlock.index());
        for (unsigned b = 0; b < m_body.blocks.size(); ++b) {
            if (m_body.blocks[b].terminator.kind != TerminatorKind::Return || isSuspendBlock.get(b))
                continue;
            BlockID block(b);
            ValueID result = m_body.at(block).terminator.value;
            if (!result)
                result = append(m_body, block, undefined());
            if (m_isGenerator) {
                ValueID completed = append(m_body, block, integer(CoroutineFrame::completedState, m_program.types.int32));
                append(m_body, block, putField(m_frame, CoroutineFrame::stateField, completed));
                m_body.at(block).terminator.value = result;
                continue;
            }
            ValueID promise = append(m_body, block, getField(m_frame, CoroutineFrame::promiseField));
            append(m_body, block, operation("ResolvePromise"_s, m_program.types.undefined, { promise, result }));
            m_body.at(block).terminator.value = append(m_body, block, undefined());
        }
    }

    Vector<BlockID> resumeBlocksBySuspendBlock() const
    {
        Vector<BlockID> result(m_body.blocks.size());
        for (auto& point : m_suspensionPoints)
            result[point.suspendBlock.index()] = point.resumeBlock;
        return result;
    }

    // Backward liveness over the rewired CFG. What is live into a resume block has no definition on the path from the dispatch block,
    // which is exactly what has to be reloaded there.
    // By block, for the first block and the resume blocks: the values, in the order of their numbers.
    Vector<Vector<uint32_t>> computeLiveAtHead()
    {
        unsigned numberOfBlocks = m_body.blocks.size();
        unsigned numberOfValues = m_body.values.size();
        static constexpr uint32_t none = std::numeric_limits<uint32_t>::max();
        Vector<Vector<uint32_t>> liveAtHead(numberOfBlocks);
        BitVector isAskedAbout(numberOfBlocks);
        isAskedAbout.set(0);
        Vector<Vector<uint32_t, 2>> predecessors(numberOfBlocks);
        // What is live after a suspension point is live before it, where it is spilled.
        for (auto& point : m_suspensionPoints) {
            isAskedAbout.set(point.resumeBlock.index());
            predecessors[point.resumeBlock.index()].append(point.suspendBlock.index());
        }
        Vector<uint32_t> definedIn;
        definedIn.fill(none, numberOfValues);
        Vector<Vector<uint32_t, 2>> usedIn(numberOfValues);
        for (unsigned b = 0; b < numberOfBlocks; ++b) {
            auto& block = m_body.blocks[b];
            auto use = [&](ValueID used) {
                if (used && (usedIn[used.index()].isEmpty() || usedIn[used.index()].last() != b))
                    usedIn[used.index()].append(b);
            };
            for (ValueID parameter : block.parameters)
                definedIn[parameter.index()] = b;
            for (ValueID id : block.values) {
                definedIn[id.index()] = b;
                m_body.at(id).forEachChild([&](ValueID child) {
                    use(child);
                });
            }
            use(block.terminator.value);
            for (auto& target : block.terminator.targets) {
                predecessors[target.block.index()].append(b);
                for (ValueID argument : target.arguments)
                    use(argument);
            }
        }
        Vector<uint32_t> lastValueLiveInto;
        lastValueLiveInto.fill(none, numberOfBlocks);
        Vector<uint32_t> worklist;
        for (unsigned value = 0; value < numberOfValues; ++value) {
            worklist.appendVector(usedIn[value]);
            while (!worklist.isEmpty()) {
                uint32_t b = worklist.takeLast();
                if (b == definedIn[value] || lastValueLiveInto[b] == value)
                    continue;
                lastValueLiveInto[b] = value;
                if (isAskedAbout.get(b))
                    liveAtHead[b].append(value);
                worklist.appendVector(predecessors[b]);
            }
        }
        return liveAtHead;
    }

    bool isRematerializable(ValueID id) const
    {
        auto& value = m_body.at(id);
        switch (value.opcode) {
        case Opcode::Undefined:
        case Opcode::Null:
        case Opcode::Boolean:
        case Opcode::Integer:
        case Opcode::Double:
        case Opcode::String:
        case Opcode::FunctionReference:
        case Opcode::BuiltinFunction:
            return true;
        // What the engine has under the name stays there. Whoever goes by the definition of an operand still finds it.
        case Opcode::GetGlobal: {
            BuiltinName name = m_program.builtinName(value.string());
            return Builtins::singleton().isGlobal(name) && !m_program.isExtended(name);
        }
        case Opcode::GetField:
            return value.child(0) == m_frame && value.layout() == m_frameLayout && m_program.at(m_frameLayout).fields[value.field()].flags & fieldIsImmutable;
        default:
            return false;
        }
    }

    // An unreachable block has no definition to offer to the blocks it jumps to.
    void clearUnreachableBlocks()
    {
        Vector<BlockID> resumeBlockOf = resumeBlocksBySuspendBlock();
        auto reachableFrom = [&](Vector<BlockID> worklist) {
            BitVector isReachable(m_body.blocks.size());
            isReachable.set(0);
            while (!worklist.isEmpty()) {
                BlockID block = worklist.takeLast();
                if (isReachable.set(block.index()))
                    continue;
                if (resumeBlockOf[block.index()])
                    worklist.append(resumeBlockOf[block.index()]);
                for (auto& target : m_body.at(block).terminator.targets)
                    worklist.append(target.block);
            }
            return isReachable;
        };
        BitVector isReachable = reachableFrom({ m_start });
        // The states keep their numbers.
        for (auto& point : m_suspensionPoints)
            point.isReachable = isReachable.get(point.suspendBlock.index());
        if (!m_isGenerator) {
            Vector<BlockID> resumeBlocks;
            for (auto& point : m_suspensionPoints) {
                if (point.isReachable)
                    resumeBlocks.append(point.resumeBlock);
            }
            isReachable = reachableFrom(WTF::move(resumeBlocks));
        }
        for (auto& point : m_suspensionPoints)
            point.isReachableInResumeFunction = isReachable.get(point.suspendBlock.index());
        for (unsigned b = 0; b < m_body.blocks.size(); ++b) {
            if (isReachable.get(b))
                continue;
            m_body.blocks[b] = Block { };
            m_body.blocks[b].terminator.kind = TerminatorKind::Unreachable;
        }
    }

    using ValueByBlock = UncheckedKeyHashMap<uint32_t, ValueID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;

    // Of one value that is kept in the frame: what stands for it where.
    struct Definitions {
        ValueID variable;
        TypeID type;
        ValueByBlock atTail;
        ValueByBlock atHead;
    };

    void spillAndReload()
    {
        clearUnreachableBlocks();
        auto liveAtHead = computeLiveAtHead();
        RELEASE_ASSERT(liveAtHead[0].isEmpty());

        unsigned numberOfOriginalValues = m_body.values.size();
        Vector<BlockID> definingBlock(numberOfOriginalValues);
        for (unsigned b = 0; b < m_body.blocks.size(); ++b) {
            for (ValueID id : m_body.blocks[b].parameters)
                definingBlock[id.index()] = BlockID(b);
            for (ValueID id : m_body.blocks[b].values)
                definingBlock[id.index()] = BlockID(b);
        }

        UncheckedKeyHashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> spillFields;
        UncheckedKeyHashMap<uint32_t, Definitions, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> definitions;
        for (auto& point : m_suspensionPoints) {
            if (!point.isReachable)
                continue;
            Vector<ValueID> reloads;
            Vector<ValueID> spills;
            for (unsigned index : liveAtHead[point.resumeBlock.index()]) {
                ValueID variable(index);
                if (variable == m_frame || variable == m_resumeMode || variable == m_resumeValue)
                    continue;
                TypeID type = m_body.at(variable).type;
                auto& variableDefinitions = definitions.ensure(index, [&] {
                    Definitions result;
                    result.variable = variable;
                    result.type = type;
                    if (definingBlock[index])
                        result.atTail.add(definingBlock[index].index(), variable);
                    return result;
                }).iterator->value;
                ValueID reload;
                if (isRematerializable(variable))
                    reload = addValue(m_body, Value(m_body.at(variable)));
                else {
                    uint32_t field = spillFields.ensure(index, [&] {
                        return addFrameField("spill"_s, type);
                    }).iterator->value;
                    reload = addValue(m_body, getField(m_frame, field));
                    m_body.at(reload).takePredictionOf(m_body.at(variable));
                    spills.append(addValue(m_body, putField(m_frame, field, variable)));
                    point.spilled.append({ variable, field });
                }
                reloads.append(reload);
                variableDefinitions.atTail.add(point.resumeBlock.index(), reload);
            }
            m_body.at(point.resumeBlock).values.insertVector(0, reloads);
            if (point.isReachableInResumeFunction)
                m_body.at(point.suspendBlock).values.insertVector(point.spillIndex, spills);
        }

        computePredecessors();
        // By value that is kept in the frame: where it is used, in the order of the blocks, and of the values in a block.
        UncheckedKeyHashMap<uint32_t, Vector<Use>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> uses;
        for (auto& entry : definitions)
            uses.add(entry.key, Vector<Use>());
        auto note = [&](ValueID used, Use use) {
            if (!used)
                return;
            auto found = uses.find(used.index());
            if (found != uses.end() && (found->value.isEmpty() || found->value.last() != use))
                found->value.append(use);
        };
        for (unsigned b = 0; b < m_body.blocks.size(); ++b) {
            for (ValueID id : m_body.blocks[b].values) {
                m_body.at(id).forEachChild([&](ValueID child) {
                    note(child, Use { BlockID(b), id });
                });
            }
            m_body.blocks[b].terminator.forEachChild([&](ValueID& child) {
                note(child, Use { BlockID(b), ValueID() });
            });
        }
        for (auto& entry : definitions)
            repairSSA(ValueID(entry.key), definingBlock[entry.key], entry.value, uses.find(entry.key)->value);

        // A value that was reloaded and not redefined since is in its spill slot already.
        for (auto& point : m_suspensionPoints) {
            m_body.at(point.suspendBlock).values.removeAllMatching([&](ValueID id) {
                auto& store = m_body.at(id);
                if (store.opcode != Opcode::PutField || store.child(0) != m_frame)
                    return false;
                auto& stored = m_body.at(store.child(1));
                return stored.opcode == Opcode::GetField && stored.child(0) == m_frame && stored.field() == store.field();
            });
        }
    }

    void computePredecessors()
    {
        m_predecessors.fill({ }, m_body.blocks.size());
        for (unsigned b = 0; b < m_body.blocks.size(); ++b) {
            for (auto& target : m_body.blocks[b].terminator.targets) {
                if (!m_predecessors[target.block.index()].contains(BlockID(b)))
                    m_predecessors[target.block.index()].append(BlockID(b));
            }
        }
    }

    ValueID definitionAtTail(Definitions& definitions, BlockID block)
    {
        auto found = definitions.atTail.find(block.index());
        if (found != definitions.atTail.end())
            return found->value;
        return definitionAtHead(definitions, block);
    }

    ValueID definitionAtHead(Definitions& definitions, BlockID block)
    {
        auto found = definitions.atHead.find(block.index());
        if (found != definitions.atHead.end())
            return found->value;
        auto& predecessors = m_predecessors[block.index()];
        RELEASE_ASSERT(!predecessors.isEmpty());
        unsigned numberOfEdges = 0;
        for (BlockID predecessor : predecessors) {
            for (auto& target : m_body.at(predecessor).terminator.targets)
                numberOfEdges += target.block == block;
        }
        if (numberOfEdges == 1) {
            ValueID definition = definitionAtTail(definitions, predecessors[0]);
            definitions.atHead.add(block.index(), definition);
            return definition;
        }
        ValueID phi = addParameter(m_body, block, definitions.type);
        m_body.at(phi).takePredictionOf(m_body.at(definitions.variable));
        definitions.atHead.add(block.index(), phi);
        for (BlockID predecessor : predecessors) {
            ValueID incoming = definitionAtTail(definitions, predecessor);
            for (auto& target : m_body.at(predecessor).terminator.targets) {
                if (target.block == block)
                    target.arguments.append(incoming);
            }
        }
        return phi;
    }

    // The value that uses it. None: the terminator of the block.
    struct Use {
        BlockID block;
        ValueID user;
        friend bool operator==(const Use&, const Use&) = default;
    };

    void repairSSA(ValueID variable, BlockID definingBlock, Definitions& definitions, const Vector<Use>& uses)
    {
        for (auto& use : uses) {
            if (use.block == definingBlock)
                continue;
            // A reload is at the head of its block, so it reaches every use in it.
            auto local = definitions.atTail.find(use.block.index());
            ValueID with = local != definitions.atTail.end() ? local->value : definitionAtHead(definitions, use.block);
            auto replace = [&](ValueID& child) {
                if (child == variable)
                    child = with;
            };
            if (use.user)
                m_body.at(use.user).forEachChild(replace);
            else
                m_body.at(use.block).terminator.forEachChild(replace);
        }
    }

    void finishDispatchBlock()
    {
        // Nothing resumes an async function that never suspends, and it has no frame to look at.
        if (!m_isGenerator && std::ranges::none_of(m_suspensionPoints, &SuspensionPoint::isReachable))
            return;
        BlockID dispatch(0u);
        ValueID state = append(m_body, dispatch, getField(m_frame, CoroutineFrame::stateField));
        auto& terminator = m_body.at(dispatch).terminator;
        terminator.kind = TerminatorKind::Switch;
        terminator.value = state;
        for (unsigned i = 0; i < m_suspensionPoints.size(); ++i) {
            terminator.cases.append(i + 1);
            terminator.targets.append(Target { m_suspensionPoints[i].resumeBlock, { } });
        }
        // The last target is the default. The frame of an async function is born suspended.
        terminator.targets.append(Target { m_start, { } });
    }

    // Behind an Await the start function has nothing: the function is resumed by a job.
    void finishStartFunction()
    {
        auto& function = m_startBody;
        BlockID first = addBlock(function);
        function.at(first) = std::exchange(function.blocks[0], Block { });
        for (auto& block : function.blocks) {
            for (auto& target : block.terminator.targets) {
                if (!target.block.index())
                    target.block = first;
            }
        }
        m_position = function.position;
        BlockID entry(0u);
        function.at(entry).terminator.kind = TerminatorKind::Jump;
        function.at(entry).terminator.targets.append(Target { first, { } });
        for (TypeID type : m_argumentTypes) {
            function.parameterTypes.append(type);
            ValueID argument = addParameter(function, entry, type);
            function.at(entry).terminator.targets[0].arguments.append(argument);
        }
        function.parameterTypes.append(m_driverType);
        ValueID promise = addParameter(function, entry, m_driverType);
        Vector<ValueID> arguments = function.at(first).parameters;
        for (ValueID argument : arguments)
            function.at(argument).immediates[0] = first.index();

        UncheckedKeyHashMap<uint32_t, unsigned, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> pointOf;
        for (unsigned i = 0; i < m_suspensionPoints.size(); ++i)
            pointOf.add(m_suspensionPoints[i].suspension.index(), i);
        for (unsigned b = 0; b < function.blocks.size(); ++b) {
            BlockID block(b);
            size_t index = function.at(block).values.findIf([&](ValueID id) {
                return function.at(id).opcode == Opcode::Await;
            });
            if (index == notFound) {
                if (function.at(block).terminator.kind != TerminatorKind::Return)
                    continue;
                ValueID result = function.at(block).terminator.value;
                if (!result)
                    result = append(function, block, undefined());
                append(function, block, operation("ResolvePromise"_s, m_program.types.undefined, { promise, result }));
                function.at(block).terminator.value = append(function, block, undefined());
                continue;
            }
            ValueID suspension = function.at(block).values[index];
            ValueID operand = function.at(suspension).child(0);
            unsigned point = pointOf.get(suspension.index());
            m_position = function.at(suspension).position;
            function.at(block).values.shrink(index);
            function.at(block).terminator = Terminator { };
            ValueID state = append(function, block, integer(point + 1, m_program.types.int32));
            Value allocation = typed(Opcode::NewObjectWithShape, m_frameType);
            allocation.immediates[0] = m_frameLayout.index();
            ValueID frame = append(function, block, WTF::move(allocation));
            append(function, block, putField(frame, CoroutineFrame::stateField, state));
            append(function, block, putField(frame, CoroutineFrame::promiseField, promise));
            for (unsigned i = 0; i < arguments.size(); ++i)
                append(function, block, putField(frame, CoroutineFrame::firstArgumentField + i, arguments[i]));
            for (auto [variable, field] : m_suspensionPoints[point].spilled)
                append(function, block, putField(frame, field, variable));
            Value closure = typed(Opcode::NewClosure, m_program.types.function);
            closure.immediates[0] = m_resume.index();
            closure.arguments.append(frame);
            ValueID resumeClosure = append(function, block, WTF::move(closure));
            append(function, block, putField(frame, CoroutineFrame::resumeClosureField, resumeClosure));
            append(function, block, operation("SuspendAsyncFunction"_s, m_program.types.undefined, { resumeClosure, operand }));
            function.at(block).terminator.kind = TerminatorKind::Return;
            function.at(block).terminator.value = append(function, block, undefined());
        }

        BitVector isReachable(function.blocks.size());
        Vector<BlockID> worklist { entry };
        while (!worklist.isEmpty()) {
            BlockID block = worklist.takeLast();
            if (isReachable.set(block.index()))
                continue;
            for (auto& target : function.at(block).terminator.targets)
                worklist.append(target.block);
        }
        for (unsigned b = 0; b < function.blocks.size(); ++b) {
            if (isReachable.get(b))
                continue;
            function.blocks[b] = Block { };
            function.blocks[b].terminator.kind = TerminatorKind::Unreachable;
        }
    }

    // What the ramp hands back is what the engine makes, whatever the function is declared to yield.
    void returnFrom(Function& ramp, BlockID block, ValueID driver)
    {
        if (ramp.resultType && !m_program.at(m_driverType).isWithin(m_program.at(ramp.resultType), m_program))
            driver = append(ramp, block, cast(driver, ramp.resultType, CastReason::Store));
        ramp.at(block).terminator.kind = TerminatorKind::Return;
        ramp.at(block).terminator.value = driver;
    }

    void buildRamp()
    {
        auto& ramp = m_program.at(m_ramp);
        m_position = ramp.position;
        BlockID entry = addBlock(ramp);
        Vector<ValueID> arguments;
        for (TypeID type : m_argumentTypes)
            arguments.append(addParameter(ramp, entry, type));

        if (!m_isGenerator) {
            BlockID done = addBlock(ramp);
            BlockID failed = addBlock(ramp);
            ValueID promise = append(ramp, entry, operation("NewPromise"_s, m_driverType, { }));
            Value start = typed(Opcode::CallDirect, m_program.types.undefined);
            start.immediates[0] = ramp.startFunction.index();
            start.arguments.appendVector(arguments);
            start.arguments.append(promise);
            append(ramp, entry, WTF::move(start));
            ramp.at(entry).terminator.kind = TerminatorKind::Invoke;
            ramp.at(entry).terminator.targets.append(Target { done, { } });
            ramp.at(entry).terminator.targets.append(Target { failed, { } });

            returnFrom(ramp, done, promise);

            ValueID error = append(ramp, failed, typed(Opcode::CatchValue, m_program.types.anything));
            append(ramp, failed, operation("RejectPromise"_s, m_program.types.undefined, { promise, error }));
            returnFrom(ramp, failed, promise);
            return;
        }

        ValueID notStarted = append(ramp, entry, integer(0, m_program.types.int32));
        Value allocation = typed(Opcode::NewObjectWithShape, m_frameType);
        allocation.immediates[0] = m_frameLayout.index();
        ValueID frame = append(ramp, entry, WTF::move(allocation));
        append(ramp, entry, putField(frame, CoroutineFrame::stateField, notStarted));
        for (unsigned i = 0; i < arguments.size(); ++i)
            append(ramp, entry, putField(frame, CoroutineFrame::firstArgumentField + i, arguments[i]));
        Value closure = typed(Opcode::NewClosure, m_program.types.function);
        closure.immediates[0] = m_resume.index();
        closure.arguments.append(frame);
        ValueID resumeClosure = append(ramp, entry, WTF::move(closure));
        append(ramp, entry, putField(frame, CoroutineFrame::resumeClosureField, resumeClosure));

        // The body does not start before the first call of next().
        ValueID generator = append(ramp, entry, operation(m_isAsync ? "NewAsyncGenerator"_s : "NewGenerator"_s, m_driverType, { resumeClosure }));
        // Resumption from an await finds the driver here.
        if (m_isAsync)
            append(ramp, entry, putField(frame, CoroutineFrame::promiseField, generator));
        if (m_startsEagerly) {
            // What it throws, the call throws.
            ValueID mode = append(ramp, entry, integer(resumeModeNormal, m_resumeModeType));
            ValueID resumeValue = append(ramp, entry, undefined());
            append(ramp, entry, callResumeFunction(frame, mode, resumeValue));
        }
        returnFrom(ramp, entry, generator);
    }

    Program& m_program;
    FunctionID m_ramp;
    FunctionID m_resume;
    Function m_body;
    Function m_startBody;
    LayoutID m_frameLayout;
    TypeID m_frameType;
    TypeID m_resumeModeType;
    // Of what the ramp hands back: the promise, or what stands for the generator.
    TypeID m_driverType;
    TypeID m_resultTypeOfResumeFunction;
    // Of what the entry block of the original takes: the captures, `this`, the parameters.
    Vector<TypeID> m_argumentTypes;
    uint32_t m_position { 0 };
    BlockID m_start;
    ValueID m_frame;
    ValueID m_resumeMode;
    ValueID m_resumeValue;
    bool m_isGenerator { false };
    bool m_isAsync { false };
    bool m_startsEagerly { false };
    Vector<SuspensionPoint> m_suspensionPoints;
    Vector<Vector<BlockID, 2>> m_predecessors;
};

} // anonymous namespace

void splitCoroutines(Program& program)
{
    unsigned numberOfFunctions = program.functions.size();
    for (unsigned i = 0; i < numberOfFunctions; ++i) {
        auto& function = program.functions[i];
        if ((function.has(Format::FunctionIsAsync) || function.has(Format::FunctionIsGenerator)) && !function.blocks.isEmpty())
            CoroutineSplitter(program, FunctionID(i)).run();
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
