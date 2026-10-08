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
// The ramp keeps the identity and the signature of the original. It allocates the coroutine frame, stores the arguments into it,
// creates the result promise and calls the resume function once.
//
// The resume function is the original body behind a switch on the frame's state. At each Await, the SSA values that are live across
// it are spilled to frame fields and the function returns. The matching resume block reloads them, and SSA form is repaired with
// block parameters. Between two suspension points values stay in SSA form, so B3 keeps them in registers.
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
        moveBodyToResumeFunction();
        createDispatchBlock();
        replaceResumeModeReads();
        splitAtSuspensionPoints();
        rewriteReturns();
        spillAndReload();
        finishDispatchBlock();
        m_program.functions.append(WTF::move(m_body));
        buildRamp();
    }

private:
    struct SuspensionPoint {
        BlockID suspendBlock;
        BlockID resumeBlock;
        // Where in the suspend block the spills go: before the store of the state.
        unsigned spillIndex { 0 };
        bool isReachable { true };
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

    Value callResumeFunction(ValueID frame, ValueID mode) const
    {
        Value value = typed(Opcode::CallDirect, m_resultTypeOfResumeFunction);
        value.immediates[0] = m_resume.index();
        value.arguments.append(frame);
        value.arguments.append(mode);
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
        m_resumeModeType = types.intern(Type::integer({ resumeModeNormal, resumeModeReturn }));
        // What NewPromise makes is a promise. What stands for a generator is an object of the run time, which has no class of the engine.
        m_driverType = m_isGenerator ? types.object : types.intern(Type::engineClass(Builtins::singleton().promiseClass()));
        // An async function settles its promise. A generator hands what it yields and what it returns to its driver.
        m_resultTypeOfResumeFunction = m_isGenerator ? types.anything : types.undefined;

        Layout layout;
        layout.kind = LayoutKind::Environment;
        layout.isCoroutineFrame = true;
        layout.type = m_frameType;
        m_program.layouts.append(WTF::move(layout));
        static_assert(!CoroutineFrame::stateField);
        RELEASE_ASSERT(addFrameField("state"_s, types.int32) == CoroutineFrame::stateField);
        RELEASE_ASSERT(addFrameField("promise"_s, m_driverType, fieldIsImmutable) == CoroutineFrame::promiseField);
        RELEASE_ASSERT(addFrameField("resumeClosure"_s, types.function, fieldIsImmutable) == CoroutineFrame::resumeClosureField);
        RELEASE_ASSERT(addFrameField("resumeValue"_s, types.anything) == CoroutineFrame::resumeValueField);
        RELEASE_ASSERT(addFrameField("resumeError"_s, types.anything) == CoroutineFrame::resumeErrorField);
    }

    void moveBodyToResumeFunction()
    {
        auto& ramp = m_program.at(m_ramp);
        m_resume = FunctionID(m_program.functions.size());
        m_body.name = ramp.name;
        m_body.module = ramp.module;
        m_body.position = ramp.position;
        m_body.kind = FunctionKind::Plain;
        m_body.flags = Format::FunctionIsUsedAsValue | (ramp.flags & Format::FunctionIsTyped);
        m_body.environment = m_frameLayout;
        m_body.parameterTypes.append(m_resumeModeType);
        m_body.resultType = m_resultTypeOfResumeFunction;
        m_body.blocks = std::exchange(ramp.blocks, { });
        m_body.values = std::exchange(ramp.values, { });
        ramp.flags &= ~(Format::FunctionIsAsync | Format::FunctionIsGenerator | Format::FunctionStartsEagerly);
        ramp.resumeFunction = m_resume;
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
        auto goOn = [&](BlockID from, BlockID to, bool mayThrow) {
            auto& terminator = m_body.at(from).terminator;
            terminator.kind = mayThrow && handler ? TerminatorKind::Invoke : TerminatorKind::Jump;
            terminator.targets.append(Target { to, { } });
            if (terminator.kind == TerminatorKind::Invoke)
                terminator.targets.append(*handler);
        };

        // The value becomes what the coroutine is resumed with. Nothing vouches for that, so it is held to the type of the value.
        BlockID resumeBlock = addBlock(m_body);
        BlockID normalBlock = addBlock(m_body);
        bool isHeld = !m_program.at(type).isAnything();
        Value resumed = getField(m_frame, CoroutineFrame::resumeValueField);
        if (isHeld)
            resumed = cast(append(m_body, normalBlock, WTF::move(resumed)), type, CastReason::Claim);
        resumed.position = m_position;
        resumed.takePredictionOf(m_body.at(suspension));
        m_body.at(suspension) = WTF::move(resumed);
        m_body.at(normalBlock).values.append(suspension);
        goOn(normalBlock, continuation, isHeld);

        if (isYield) {
            // The front end dispatches on the resume mode itself.
            goOn(resumeBlock, normalBlock, false);
        } else {
            BlockID throwBlock = addBlock(m_body);
            m_body.at(throwBlock).terminator.value = append(m_body, throwBlock, getField(m_frame, CoroutineFrame::resumeErrorField));
            m_body.at(throwBlock).terminator.kind = TerminatorKind::Throw;
            if (handler)
                m_body.at(throwBlock).terminator.targets.append(*handler);

            Value isThrow = typed(Opcode::StrictEq, m_program.types.boolean);
            isThrow.children[0] = m_resumeMode;
            isThrow.children[1] = append(m_body, resumeBlock, integer(resumeModeThrow, m_resumeModeType));
            m_body.at(resumeBlock).terminator.value = append(m_body, resumeBlock, WTF::move(isThrow));
            m_body.at(resumeBlock).terminator.kind = TerminatorKind::Branch;
            m_body.at(resumeBlock).terminator.targets.append(Target { throwBlock, { } });
            m_body.at(resumeBlock).terminator.targets.append(Target { normalBlock, { } });
        }

        m_suspensionPoints.append({ suspendBlock, resumeBlock, static_cast<unsigned>(m_body.at(suspendBlock).values.size()) });
        ValueID state = append(m_body, suspendBlock, integer(m_suspensionPoints.size(), m_program.types.int32));
        append(m_body, suspendBlock, putField(m_frame, CoroutineFrame::stateField, state));
        ValueID returned = operand;
        if (!isYield) {
            // What goes wrong with what is awaited is not thrown here: the function is resumed with it.
            ValueID closure = append(m_body, suspendBlock, getField(m_frame, CoroutineFrame::resumeClosureField));
            append(m_body, suspendBlock, operation("Await"_s, m_program.types.undefined, { closure, operand }));
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
    Vector<BitVector> computeLiveAtHead()
    {
        unsigned numberOfBlocks = m_body.blocks.size();
        Vector<BitVector> liveAtHead(numberOfBlocks);
        // What is live after a suspension point is live before it, where it is spilled.
        Vector<BlockID> resumeBlockOf = resumeBlocksBySuspendBlock();
        bool changed = true;
        while (changed) {
            changed = false;
            for (unsigned b = numberOfBlocks; b--;) {
                auto& block = m_body.blocks[b];
                BitVector live;
                if (resumeBlockOf[b])
                    live.merge(liveAtHead[resumeBlockOf[b].index()]);
                for (auto& target : block.terminator.targets) {
                    live.merge(liveAtHead[target.block.index()]);
                    for (ValueID argument : target.arguments)
                        live.set(argument.index());
                }
                if (block.terminator.value)
                    live.set(block.terminator.value.index());
                for (unsigned i = block.values.size(); i--;) {
                    ValueID id = block.values[i];
                    live.clear(id.index());
                    m_body.at(id).forEachChild([&](ValueID child) {
                        live.set(child.index());
                    });
                }
                for (ValueID parameter : block.parameters)
                    live.clear(parameter.index());
                if (live != liveAtHead[b]) {
                    liveAtHead[b] = WTF::move(live);
                    changed = true;
                }
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
        BitVector isReachable(m_body.blocks.size());
        Vector<BlockID> resumeBlockOf = resumeBlocksBySuspendBlock();
        Vector<BlockID> worklist { m_start };
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
        // The states keep their numbers.
        for (auto& point : m_suspensionPoints)
            point.isReachable = isReachable.get(point.suspendBlock.index());
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
                if (variable == m_frame || variable == m_resumeMode)
                    continue;
                TypeID type = m_body.at(variable).type;
                auto& variableDefinitions = definitions.ensure(index, [&] {
                    Definitions result;
                    result.variable = variable;
                    result.type = type;
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
                }
                reloads.append(reload);
                variableDefinitions.atTail.add(point.resumeBlock.index(), reload);
            }
            m_body.at(point.resumeBlock).values.insertVector(0, reloads);
            m_body.at(point.suspendBlock).values.insertVector(point.spillIndex, spills);
        }

        computePredecessors();
        for (auto& entry : definitions)
            repairSSA(ValueID(entry.key), definingBlock[entry.key], entry.value);

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

    void repairSSA(ValueID variable, BlockID definingBlock, Definitions& definitions)
    {
        for (unsigned b = 0; b < m_body.blocks.size(); ++b) {
            BlockID block(b);
            if (block == definingBlock)
                continue;
            // A reload is at the head of its block, so it reaches every use in it.
            auto local = definitions.atTail.find(b);
            ValueID localDefinition = local != definitions.atTail.end() ? local->value : ValueID();
            auto replacement = [&]() -> ValueID {
                return localDefinition ? localDefinition : definitionAtHead(definitions, block);
            };
            unsigned numberOfValues = m_body.blocks[b].values.size();
            for (unsigned i = 0; i < numberOfValues; ++i) {
                ValueID id = m_body.blocks[b].values[i];
                bool uses = false;
                m_body.at(id).forEachChild([&](ValueID child) {
                    uses |= child == variable;
                });
                if (!uses)
                    continue;
                ValueID with = replacement();
                m_body.at(id).forEachChild([&](ValueID& child) {
                    if (child == variable)
                        child = with;
                });
            }
            bool uses = false;
            m_body.blocks[b].terminator.forEachChild([&](ValueID& child) {
                uses |= child == variable;
            });
            if (!uses)
                continue;
            ValueID with = replacement();
            m_body.blocks[b].terminator.forEachChild([&](ValueID& child) {
                if (child == variable)
                    child = with;
            });
        }
    }

    void finishDispatchBlock()
    {
        BlockID dispatch(0u);
        ValueID state = append(m_body, dispatch, getField(m_frame, CoroutineFrame::stateField));
        auto& terminator = m_body.at(dispatch).terminator;
        terminator.kind = TerminatorKind::Switch;
        terminator.value = state;
        for (unsigned i = 0; i < m_suspensionPoints.size(); ++i) {
            terminator.cases.append(i + 1);
            terminator.targets.append(Target { m_suspensionPoints[i].resumeBlock, { } });
        }
        // The last target is the default.
        terminator.targets.append(Target { m_start, { } });
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

        ValueID notStarted = append(ramp, entry, integer(0, m_program.types.int32));
        Value allocation = typed(Opcode::NewObjectWithShape, m_frameType);
        allocation.immediates[0] = m_frameLayout.index();
        ValueID frame = append(ramp, entry, WTF::move(allocation));
        append(ramp, entry, putField(frame, CoroutineFrame::stateField, notStarted));
        for (unsigned i = 0; i < arguments.size(); ++i)
            append(ramp, entry, putField(frame, CoroutineFrame::firstArgumentField + i, arguments[i]));
        Value closure = typed(Opcode::NewClosure, m_program.types.function);
        closure.immediates[0] = m_resume.index();
        closure.children[0] = frame;
        ValueID resumeClosure = append(ramp, entry, WTF::move(closure));
        append(ramp, entry, putField(frame, CoroutineFrame::resumeClosureField, resumeClosure));

        if (m_isGenerator) {
            // The body does not start before the first call of next().
            ValueID generator = append(ramp, entry, operation(m_isAsync ? "NewAsyncGenerator"_s : "NewGenerator"_s, m_driverType, { resumeClosure }));
            // Resumption from an await finds the driver here.
            if (m_isAsync)
                append(ramp, entry, putField(frame, CoroutineFrame::promiseField, generator));
            if (m_startsEagerly) {
                // What it throws, the call throws.
                ValueID mode = append(ramp, entry, integer(resumeModeNormal, m_resumeModeType));
                append(ramp, entry, callResumeFunction(frame, mode));
            }
            returnFrom(ramp, entry, generator);
            return;
        }

        BlockID done = addBlock(ramp);
        BlockID failed = addBlock(ramp);
        ValueID promise = append(ramp, entry, operation("NewPromise"_s, m_driverType, { }));
        append(ramp, entry, putField(frame, CoroutineFrame::promiseField, promise));
        ValueID mode = append(ramp, entry, integer(resumeModeNormal, m_resumeModeType));
        append(ramp, entry, callResumeFunction(frame, mode));
        ramp.at(entry).terminator.kind = TerminatorKind::Invoke;
        ramp.at(entry).terminator.targets.append(Target { done, { } });
        ramp.at(entry).terminator.targets.append(Target { failed, { } });

        returnFrom(ramp, done, promise);

        ValueID error = append(ramp, failed, typed(Opcode::CatchValue, m_program.types.anything));
        append(ramp, failed, operation("RejectPromise"_s, m_program.types.undefined, { promise, error }));
        returnFrom(ramp, failed, promise);
    }

    Program& m_program;
    FunctionID m_ramp;
    FunctionID m_resume;
    Function m_body;
    LayoutID m_frameLayout;
    TypeID m_frameType;
    TypeID m_resumeModeType;
    // Of what the ramp hands back: the promise, or what stands for the generator.
    TypeID m_driverType;
    TypeID m_resultTypeOfResumeFunction;
    // Of what the entry block of the original takes: the environment, `this`, the parameters.
    Vector<TypeID> m_argumentTypes;
    uint32_t m_position { 0 };
    BlockID m_start;
    ValueID m_frame;
    ValueID m_resumeMode;
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
