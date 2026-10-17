/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedPasses.h"

#if ENABLE(AOT)

#include "AOTTypedData.h"
#include "AOTTypedObject.h"
#include "JSCInlines.h"
#include "JSGenerator.h"

namespace JSC { namespace AOT { namespace Typed {

namespace {

class GeneratorLoopFusion {
public:
    GeneratorLoopFusion(const Program& program, Function& function)
        : m_program(program)
        , m_function(function)
    {
    }

    unsigned run()
    {
        forgetDeclarationsOfCreatedGenerators();
        Vector<ValueID> opens;
        for (auto& block : m_function.blocks) {
            for (ValueID id : block.values) {
                if (at(id).opcode == Opcode::IteratorOpen)
                    opens.append(id);
            }
        }
        unsigned changed = 0;
        for (ValueID open : opens)
            changed += fuse(open);
        Vector<ValueID> calls;
        for (auto& block : m_function.blocks) {
            for (ValueID id : block.values) {
                if (at(id).opcode == Opcode::CallDirect && !at(id).isJumpToContinuation() && m_program.at(at(id).function()).resumeFunction)
                    calls.append(id);
            }
        }
        for (ValueID call : calls)
            changed += fuseSteps(call) || fuseSpread(call);
        return changed;
    }

    unsigned stepWhatIsNotFused()
    {
        unsigned changed = stepDeclaredGeneratorsInPlace();
        return changed + keepIteratorResultsInParts();
    }

private:
    static constexpr int64_t resumeModeNext = 0;
    static constexpr int64_t resumeModeReturn = 2;

    struct Site {
        BlockID block;
        unsigned index { 0 };
    };

    struct Cut {
        Target after;
        std::optional<Target> handler;
    };

    struct Loop {
        ValueID created;
        Vector<ValueID, 2> casts;
        ValueID hasNext;
        ValueID value;
        Vector<ValueID, 4> closes;
    };

    struct Frame {
        ValueID allocation;
        Vector<std::pair<uint32_t, uint32_t>, 8> arguments;
    };

    Value& at(ValueID id) { return m_function.at(id); }
    Block& at(BlockID id) { return m_function.at(id); }

    std::optional<Loop> loopOver(ValueID open)
    {
        Loop loop;
        loop.created = at(open).child(0);
        while (at(loop.created).opcode == Opcode::Cast) {
            if (!m_program.at(at(loop.created).type).anyObject)
                return std::nullopt;
            loop.casts.append(loop.created);
            loop.created = at(loop.created).child(0);
        }
        if (at(loop.created).opcode != Opcode::CallDirect || at(loop.created).isJumpToContinuation())
            return std::nullopt;
        if (!frameAllocatedBy(m_program.at(at(loop.created).function())))
            return std::nullopt;
        bool isOnlyIterated = true;
        auto isGenerator = [&](ValueID id) {
            return id == loop.created || loop.casts.contains(id);
        };
        for (auto& block : m_function.blocks) {
            for (ValueID id : block.values) {
                auto& user = at(id);
                user.forEachChild([&](ValueID& child) {
                    if (isGenerator(child)) {
                        isOnlyIterated &= id == open || loop.casts.contains(id);
                        return;
                    }
                    if (child != open)
                        return;
                    isOnlyIterated &= &child == &user.children[0];
                    if (user.opcode == Opcode::IteratorHasNext && !loop.hasNext)
                        loop.hasNext = id;
                    else if (user.opcode == Opcode::IteratorValue && !loop.value)
                        loop.value = id;
                    else if (user.opcode == Opcode::IteratorClose)
                        loop.closes.append(id);
                    else
                        isOnlyIterated = false;
                });
            }
            block.terminator.forEachChild([&](ValueID& child) {
                isOnlyIterated &= child != open && !isGenerator(child);
            });
        }
        if (!isOnlyIterated || !loop.hasNext || (loop.value && !m_program.at(at(loop.value).type).isAnything()))
            return std::nullopt;
        return loop;
    }

    std::optional<Frame> frameAllocatedBy(const Function& ramp) const
    {
        if (!ramp.resumeFunction || ramp.startFunction || (ramp.declaredAs & (functionIsAsync | functionIsGenerator)) != functionIsGenerator || ramp.blocks.size() != 1)
            return std::nullopt;
        Frame frame;
        for (ValueID id : ramp.blocks[0].values) {
            auto& value = ramp.at(id);
            switch (value.opcode) {
            case Opcode::Integer:
                break;
            case Opcode::NewObjectWithShape:
                if (frame.allocation)
                    return std::nullopt;
                frame.allocation = id;
                break;
            case Opcode::PutField: {
                auto& stored = ramp.at(value.child(1));
                if (value.child(0) != frame.allocation)
                    return std::nullopt;
                if (stored.opcode == Opcode::Parameter)
                    frame.arguments.append({ value.field(), stored.parameterIndex() });
                else if (value.field() != CoroutineFrame::stateField && value.field() != CoroutineFrame::resumeClosureField)
                    return std::nullopt;
                break;
            }
            case Opcode::NewClosure:
                if (value.function() != ramp.resumeFunction)
                    return std::nullopt;
                break;
            case Opcode::Operation:
                if (m_program.operationOf(value) != Operations::NewGenerator)
                    return std::nullopt;
                break;
            default:
                return std::nullopt;
            }
        }
        if (!frame.allocation || ramp.blocks[0].terminator.kind != TerminatorKind::Return)
            return std::nullopt;
        return frame;
    }

    bool mayRepeatWithoutAllocation(ValueID use, ValueID allocationSite)
    {
        BlockID from = siteOf(use).block;
        BlockID allocationBlock = siteOf(allocationSite).block;
        if (from == allocationBlock)
            return false;
        BitVector seen(m_function.blocks.size());
        Vector<BlockID, 16> worklist { from };
        while (!worklist.isEmpty()) {
            for (auto& target : at(worklist.takeLast()).terminator.targets) {
                if (target.block == from)
                    return true;
                if (target.block != allocationBlock && !seen.set(target.block.index()))
                    worklist.append(target.block);
            }
        }
        return false;
    }

    Site siteOf(ValueID id)
    {
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            size_t index = m_function.blocks[b].values.find(id);
            if (index != notFound)
                return { BlockID(b), static_cast<unsigned>(index) };
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    Value make(Opcode opcode, TypeID type, const Value& like) const
    {
        Value value;
        value.opcode = opcode;
        value.type = type;
        value.rep = repOf(m_program.at(type));
        value.position = like.position;
        value.inlineFrame = like.inlineFrame;
        return value;
    }

    ValueID append(BlockID block, Value&& value)
    {
        ValueID id(m_function.values.size());
        m_function.values.append(WTF::move(value));
        at(block).values.append(id);
        return id;
    }

    ValueID appendInteger(BlockID block, int64_t number, TypeID type, const Value& like)
    {
        Value value = make(Opcode::Integer, type, like);
        value.setBits(static_cast<uint64_t>(number));
        return append(block, WTF::move(value));
    }

    ValueID appendState(BlockID block, const Value& like)
    {
        Value value = make(Opcode::GetField, m_program.at(m_layout).fields[CoroutineFrame::stateField].type, like);
        value.children[0] = m_frame;
        value.immediates = { m_layout.index(), CoroutineFrame::stateField };
        return append(block, WTF::move(value));
    }

    ValueID appendCompare(BlockID block, Opcode opcode, ValueID left, ValueID right, const Value& like)
    {
        Value value = make(opcode, m_program.types.boolean, like);
        value.children[0] = left;
        value.children[1] = right;
        return append(block, WTF::move(value));
    }

    ValueID appendResume(BlockID block, int64_t mode, const Value& like, ValueID sent = { })
    {
        auto& resume = m_program.at(m_resume);
        ValueID modeValue = appendInteger(block, mode, resume.at(resume.blocks[0].parameters[1]).type, like);
        if (!sent)
            sent = append(block, make(Opcode::Undefined, m_program.types.undefined, like));
        Value call = make(Opcode::CallDirect, resume.resultType ? resume.resultType : m_program.types.anything, like);
        call.immediates = { m_resume.index(), m_inliningDepth };
        call.arguments = { m_frame, modeValue, sent };
        return append(block, WTF::move(call));
    }

    BlockID addBlock()
    {
        m_function.blocks.append(Block { });
        return BlockID(m_function.blocks.size() - 1);
    }

    void jump(BlockID block, const Target& target, const std::optional<Target>& handler = std::nullopt)
    {
        auto& terminator = at(block).terminator;
        terminator = Terminator { };
        terminator.kind = handler ? TerminatorKind::Invoke : TerminatorKind::Jump;
        terminator.targets.append(target);
        if (handler)
            terminator.targets.append(*handler);
    }

    Cut cutAt(Site site)
    {
        Cut cut;
        if (site.index + 1 == at(site.block).values.size() && at(site.block).terminator.kind == TerminatorKind::Invoke) {
            cut.after = at(site.block).terminator.targets[0];
            cut.handler = at(site.block).terminator.targets[1];
        } else {
            BlockID rest = addBlock();
            auto& from = at(site.block);
            auto& to = at(rest);
            for (unsigned i = site.index + 1; i < from.values.size(); ++i)
                to.values.append(from.values[i]);
            to.terminator = std::exchange(from.terminator, Terminator { });
            cut.after = Target { rest, { } };
        }
        at(site.block).values.shrink(site.index);
        at(site.block).terminator = Terminator { };
        return cut;
    }

    void remove(ValueID id)
    {
        Site site = siteOf(id);
        auto& block = at(site.block);
        block.values.removeAt(site.index);
        if (site.index == block.values.size() && block.terminator.kind == TerminatorKind::Invoke) {
            block.terminator.kind = TerminatorKind::Jump;
            block.terminator.targets.shrink(1);
        }
        at(id) = make(Opcode::Undefined, m_program.types.undefined, Value(at(id)));
    }

    void makeFrame(const Function& ramp, const Frame& frame)
    {
        Site site = siteOf(m_frame);
        Value call = at(m_frame);
        Cut cut = cutAt(site);
        ValueID notStarted = appendInteger(site.block, 0, m_program.types.int32, call);
        Value allocation = make(Opcode::NewObjectWithShape, ramp.at(frame.allocation).type, call);
        allocation.immediates = { m_layout.index(), m_function.allocationOf(Allocation(ramp.allocations[ramp.at(frame.allocation).allocation()])) };
        at(m_frame) = WTF::move(allocation);
        at(site.block).values.append(m_frame);
        auto store = [&](uint32_t field, ValueID stored) {
            Value value = make(Opcode::PutField, m_program.types.undefined, call);
            value.children[0] = m_frame;
            value.children[1] = stored;
            value.immediates = { m_layout.index(), field };
            append(site.block, WTF::move(value));
        };
        store(CoroutineFrame::stateField, notStarted);
        for (auto [field, parameter] : frame.arguments)
            store(field, call.arguments[parameter]);
        jump(site.block, cut.after);
    }

    ValueID step(ValueID hasNext)
    {
        Site site = siteOf(hasNext);
        Value like = at(hasNext);
        Cut cut = cutAt(site);
        ValueID returnedValue = appendResume(site.block, resumeModeNext, like);
        BlockID resumed = addBlock();
        jump(site.block, Target { resumed, { } }, cut.handler);
        ValueID state = appendState(resumed, like);
        ValueID isDone = appendCompare(resumed, Opcode::StrictEq, state, appendInteger(resumed, CoroutineFrame::completedState, m_program.types.int32, like), like);
        Value isNotDone = make(Opcode::LogicalNot, m_program.types.boolean, like);
        isNotDone.children[0] = isDone;
        at(hasNext) = WTF::move(isNotDone);
        at(resumed).values.append(hasNext);
        jump(resumed, cut.after);
        return returnedValue;
    }

    void close(ValueID id)
    {
        Site site = siteOf(id);
        Value like = at(id);
        Cut cut = cutAt(site);
        ValueID state = appendState(site.block, like);
        ValueID isSuspended = appendCompare(site.block, Opcode::Greater, state, appendInteger(site.block, 0, m_program.types.int32, like), like);
        BlockID returns = addBlock();
        auto& branch = at(site.block).terminator;
        branch.kind = TerminatorKind::Branch;
        branch.value = isSuspended;
        branch.targets.append(Target { returns, { } });
        branch.targets.append(cut.after);
        appendResume(returns, resumeModeReturn, like);
        if (like.immediates[0]) {
            BlockID dropped = addBlock();
            append(dropped, make(Opcode::CatchValue, m_program.types.anything, like));
            jump(dropped, cut.after);
            cut.handler = Target { dropped, { } };
        }
        jump(returns, cut.after, cut.handler);
        at(id) = make(Opcode::Undefined, m_program.types.undefined, like);
    }

    struct Steps {
        Vector<ValueID, 4> calls;
        ValueID merged;
        Vector<ValueID, 4> doneReads;
        Vector<ValueID, 4> valueReads;
    };

    std::optional<Steps> stepsOf(ValueID created)
    {
        auto next = m_program.find(asBytes("next"_s.span8()));
        auto done = m_program.find(asBytes("done"_s.span8()));
        auto value = m_program.find(asBytes("value"_s.span8()));
        if (!next || !done || !value)
            return std::nullopt;
        Steps steps;
        bool isOnlyStepped = true;
        for (auto& block : m_function.blocks) {
            for (ValueID id : block.values) {
                auto& user = at(id);
                user.forEachChild([&](ValueID& child) {
                    if (child != created)
                        return;
                    isOnlyStepped &= user.opcode == Opcode::CallMethod && &child == &user.children[0] && user.string() == *next && user.arguments.size() <= 1;
                    steps.calls.append(id);
                });
            }
            block.terminator.forEachChild([&](ValueID& child) {
                isOnlyStepped &= child != created;
            });
        }
        if (!isOnlyStepped || steps.calls.isEmpty())
            return std::nullopt;
        for (auto& block : m_function.blocks) {
            if (block.terminator.value)
                isOnlyStepped &= !steps.calls.contains(block.terminator.value);
            for (auto& target : block.terminator.targets) {
                for (unsigned i = 0; i < target.arguments.size(); ++i) {
                    if (!steps.calls.contains(target.arguments[i]))
                        continue;
                    ValueID parameter = at(target.block).parameters[i];
                    isOnlyStepped &= !steps.merged || steps.merged == parameter;
                    steps.merged = parameter;
                }
            }
        }
        if (steps.merged) {
            BlockID mergeBlock = at(steps.merged).parameterBlock();
            unsigned index = at(steps.merged).parameterIndex();
            for (auto& block : m_function.blocks) {
                for (auto& target : block.terminator.targets) {
                    if (target.block == mergeBlock)
                        isOnlyStepped &= index < target.arguments.size() && steps.calls.contains(target.arguments[index]);
                }
            }
        }
        auto isResult = [&](ValueID id) {
            return id == steps.merged || steps.calls.contains(id);
        };
        for (auto& block : m_function.blocks) {
            for (ValueID id : block.values) {
                auto& user = at(id);
                user.forEachChild([&](ValueID& child) {
                    if (!isResult(child))
                        return;
                    if (user.opcode == Opcode::GetById && user.string() == *done)
                        steps.doneReads.append(id);
                    else if (user.opcode == Opcode::GetById && user.string() == *value)
                        steps.valueReads.append(id);
                    else
                        isOnlyStepped = false;
                });
            }
            block.terminator.forEachChild([&](ValueID& child) {
                isOnlyStepped &= child != steps.merged;
            });
        }
        if (!isOnlyStepped)
            return std::nullopt;
        return steps;
    }

    ValueID addParameter(BlockID block, TypeID type, const Value& like)
    {
        Value parameter = make(Opcode::Parameter, type, like);
        parameter.immediates = { block.index(), static_cast<uint32_t>(at(block).parameters.size()) };
        ValueID id(m_function.values.size());
        m_function.values.append(WTF::move(parameter));
        at(block).parameters.append(id);
        return id;
    }

    std::pair<ValueID, ValueID> emitManualStep(ValueID call)
    {
        Site site = siteOf(call);
        Value like = at(call);
        Cut cut = cutAt(site);
        BlockID runs = addBlock();
        BlockID resumed = addBlock();
        BlockID isOver = addBlock();
        BlockID stepped = addBlock();
        ValueID value = addParameter(stepped, m_program.types.anything, like);
        ValueID isDone = addParameter(stepped, m_program.types.boolean, like);
        auto appendIsCompleted = [&](BlockID block) {
            ValueID state = appendState(block, like);
            return appendCompare(block, Opcode::StrictEq, state, appendInteger(block, CoroutineFrame::completedState, m_program.types.int32, like), like);
        };
        ValueID wasCompleted = appendIsCompleted(site.block);
        auto& branch = at(site.block).terminator;
        branch.kind = TerminatorKind::Branch;
        branch.value = wasCompleted;
        branch.targets.append(Target { isOver, { } });
        branch.targets.append(Target { runs, { } });

        ValueID nothing = append(isOver, make(Opcode::Undefined, m_program.types.undefined, like));
        Value yes = make(Opcode::Boolean, m_program.types.boolean, like);
        yes.immediates[0] = 1;
        ValueID over = append(isOver, WTF::move(yes));
        jump(isOver, Target { stepped, { nothing, over } });

        ValueID returnedValue = appendResume(runs, resumeModeNext, like, like.arguments.isEmpty() ? ValueID() : like.arguments[0]);
        if (cut.handler) {
            BlockID failed = addBlock();
            jump(runs, Target { resumed, { } }, Target { failed, { } });
            ValueID error = append(failed, make(Opcode::CatchValue, m_program.types.anything, like));
            Value store = make(Opcode::PutField, m_program.types.undefined, like);
            store.children[0] = m_frame;
            store.children[1] = appendInteger(failed, CoroutineFrame::completedState, m_program.types.int32, like);
            store.immediates = { m_layout.index(), CoroutineFrame::stateField };
            append(failed, WTF::move(store));
            auto& rethrow = at(failed).terminator;
            rethrow.kind = TerminatorKind::Throw;
            rethrow.value = error;
            rethrow.targets.append(*cut.handler);
        } else
            jump(runs, Target { resumed, { } });
        ValueID isCompleted = appendIsCompleted(resumed);
        jump(resumed, Target { stepped, { returnedValue, isCompleted } });

        at(call) = make(Opcode::Undefined, m_program.types.undefined, like);
        at(stepped).values.append(call);
        jump(stepped, cut.after);
        return { value, isDone };
    }

    bool fuseSteps(ValueID created)
    {
        if (at(created).opcode != Opcode::CallDirect)
            return false;
        auto& ramp = m_program.at(at(created).function());
        auto frame = frameAllocatedBy(ramp);
        if (!frame || at(created).arguments.size() != ramp.blocks[0].parameters.size())
            return false;
        auto steps = stepsOf(created);
        if (!steps)
            return false;
        m_frame = created;
        m_layout = ramp.at(frame->allocation).layout();
        m_resume = ramp.resumeFunction;
        m_inliningDepth = at(created).immediates[1];
        makeFrame(ramp, *frame);
        UncheckedKeyHashMap<uint32_t, std::pair<ValueID, ValueID>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> parts;
        for (ValueID call : steps->calls)
            parts.add(call.index(), emitManualStep(call));
        if (steps->merged) {
            BlockID mergeBlock = at(steps->merged).parameterBlock();
            unsigned index = at(steps->merged).parameterIndex();
            Value like = at(steps->merged);
            ValueID value = addParameter(mergeBlock, m_program.types.anything, like);
            ValueID isDone = addParameter(mergeBlock, m_program.types.boolean, like);
            for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
                for (unsigned t = 0; t < m_function.blocks[b].terminator.targets.size(); ++t) {
                    if (m_function.blocks[b].terminator.targets[t].block != mergeBlock)
                        continue;
                    auto [stepValue, stepIsDone] = parts.get(m_function.blocks[b].terminator.targets[t].arguments[index].index());
                    auto& arguments = m_function.blocks[b].terminator.targets[t].arguments;
                    arguments.append(stepValue);
                    arguments.append(stepIsDone);
                }
            }
            parts.add(steps->merged.index(), std::pair { value, isDone });
        }
        for (ValueID read : steps->doneReads) {
            ValueID isDone = parts.get(at(read).child(0).index()).second;
            auto replace = [&](ValueID& child) {
                if (child == read)
                    child = isDone;
            };
            for (auto& block : m_function.blocks) {
                for (ValueID id : block.values)
                    at(id).forEachChild(replace);
                block.terminator.forEachChild(replace);
            }
            remove(read);
        }
        for (ValueID read : steps->valueReads) {
            Value cast = make(Opcode::Cast, at(read).type, Value(at(read)));
            cast.children[0] = parts.get(at(read).child(0).index()).first;
            cast.immediates[1] = static_cast<uint32_t>(CastReason::Claim);
            cast.copyPredictionFrom(at(read));
            at(read) = WTF::move(cast);
        }
        return true;
    }

    bool fuseSpread(ValueID created)
    {
        auto push = m_program.find(asBytes("push"_s.span8()));
        if (at(created).opcode != Opcode::CallDirect || !push)
            return false;
        auto& ramp = m_program.at(at(created).function());
        auto frame = frameAllocatedBy(ramp);
        if (!frame || at(created).arguments.size() != ramp.blocks[0].parameters.size())
            return false;
        Vector<ValueID, 2> aliases { created };
        for (bool isGrowing = true; isGrowing;) {
            isGrowing = false;
            for (auto& block : m_function.blocks) {
                for (ValueID id : block.values) {
                    if (at(id).opcode == Opcode::Cast && m_program.at(at(id).type).anyObject && aliases.contains(at(id).child(0)) && !aliases.contains(id)) {
                        aliases.append(id);
                        isGrowing = true;
                    }
                }
            }
        }
        ValueID spread;
        bool isOnlySpread = true;
        for (auto& block : m_function.blocks) {
            for (ValueID id : block.values) {
                auto& user = at(id);
                user.forEachChild([&](ValueID& child) {
                    if (!aliases.contains(child) || aliases.contains(id))
                        return;
                    if (user.opcode == Opcode::Operation && m_program.operationOf(user) == Operations::AppendSpread && user.arguments.size() == 2 && &child == &user.arguments[1] && !spread)
                        spread = id;
                    else
                        isOnlySpread = false;
                });
            }
            block.terminator.forEachChild([&](ValueID& child) {
                isOnlySpread &= !aliases.contains(child);
            });
        }
        if (!isOnlySpread || !spread || mayRepeatWithoutAllocation(spread, created))
            return false;
        m_frame = created;
        m_layout = ramp.at(frame->allocation).layout();
        m_resume = ramp.resumeFunction;
        m_inliningDepth = at(created).immediates[1];
        makeFrame(ramp, *frame);
        for (unsigned i = 1; i < aliases.size(); ++i)
            remove(aliases[i]);
        Site site = siteOf(spread);
        Value like = at(spread);
        Cut cut = cutAt(site);
        BlockID header = addBlock();
        BlockID resumed = addBlock();
        BlockID pushes = addBlock();
        BlockID exit = addBlock();
        jump(site.block, Target { header, { } });
        append(header, make(Opcode::LoopHint, m_program.types.undefined, like));
        ValueID returnedValue = appendResume(header, resumeModeNext, like);
        jump(header, Target { resumed, { } }, cut.handler);
        ValueID state = appendState(resumed, like);
        ValueID isDone = appendCompare(resumed, Opcode::StrictEq, state, appendInteger(resumed, CoroutineFrame::completedState, m_program.types.int32, like), like);
        auto& branch = at(resumed).terminator;
        branch.kind = TerminatorKind::Branch;
        branch.value = isDone;
        branch.targets.append(Target { exit, { } });
        branch.targets.append(Target { pushes, { } });
        Value call = make(Opcode::CallMethod, m_program.types.anything, like);
        call.children[0] = like.arguments[0];
        call.immediates = { push->index(), m_inliningDepth };
        call.arguments = { returnedValue };
        append(pushes, WTF::move(call));
        jump(pushes, Target { header, { } }, cut.handler);
        Value array = make(Opcode::Cast, like.type, like);
        array.children[0] = like.arguments[0];
        array.immediates[1] = static_cast<uint32_t>(CastReason::Claim);
        at(spread) = WTF::move(array);
        at(exit).values.append(spread);
        jump(exit, cut.after, cut.handler);
        return true;
    }

    bool fuse(ValueID open)
    {
        auto loop = loopOver(open);
        if (!loop)
            return false;
        auto& ramp = m_program.at(at(loop->created).function());
        auto frame = frameAllocatedBy(ramp);
        if (!frame || at(loop->created).arguments.size() != ramp.blocks[0].parameters.size() || mayRepeatWithoutAllocation(open, loop->created))
            return false;
        m_frame = loop->created;
        m_layout = ramp.at(frame->allocation).layout();
        m_resume = ramp.resumeFunction;
        m_inliningDepth = at(loop->created).immediates[1];
        makeFrame(ramp, *frame);
        for (ValueID cast : loop->casts)
            remove(cast);
        remove(open);
        ValueID returnedValue = step(loop->hasNext);
        if (loop->value) {
            auto replace = [&](ValueID& child) {
                if (child == loop->value)
                    child = returnedValue;
            };
            for (auto& block : m_function.blocks) {
                for (ValueID id : block.values)
                    at(id).forEachChild(replace);
                block.terminator.forEachChild(replace);
            }
            remove(loop->value);
        }
        for (ValueID id : loop->closes)
            close(id);
        return true;
    }

    bool isCreatedGenerator(ValueID id)
    {
        auto& value = at(id);
        if (value.opcode != Opcode::CallDirect || value.isJumpToContinuation())
            return false;
        auto& ramp = m_program.at(value.function());
        return ramp.resumeFunction && !ramp.startFunction && (ramp.declaredAs & (functionIsAsync | functionIsGenerator)) == functionIsGenerator;
    }

    bool isDeclaredGenerator(ValueID id) { return m_program.operationOf(at(id)) == Operations::DeclaredGenerator; }

    void replaceUses(ValueID from, ValueID to)
    {
        auto replace = [&](ValueID& child) {
            if (child == from)
                child = to;
        };
        for (auto& block : m_function.blocks) {
            for (ValueID id : block.values)
                at(id).forEachChild(replace);
            block.terminator.forEachChild(replace);
        }
    }

    void forgetDeclarationsOfCreatedGenerators()
    {
        Vector<ValueID, 4> declarations;
        for (auto& block : m_function.blocks) {
            for (ValueID id : block.values) {
                if (isDeclaredGenerator(id) && isCreatedGenerator(at(id).arguments[0]))
                    declarations.append(id);
            }
        }
        for (ValueID id : declarations) {
            replaceUses(id, at(id).arguments[0]);
            remove(id);
        }
    }

    ValueID appendOperation(BlockID block, BuiltinID which, TypeID type, const Value& like, std::initializer_list<ValueID> arguments)
    {
        Value value = make(Opcode::Operation, type, like);
        value.immediates[0] = which.index();
        for (ValueID argument : arguments)
            value.arguments.append(argument);
        return append(block, WTF::move(value));
    }

    Value makeBoolean(bool boolean, const Value& like)
    {
        Value value = make(Opcode::Boolean, m_program.types.boolean, like);
        value.immediates[0] = boolean;
        return value;
    }

    ValueID prepend(BlockID block, Value&& value)
    {
        ValueID id(m_function.values.size());
        m_function.values.append(WTF::move(value));
        at(block).values.insert(0, id);
        return id;
    }

    void branch(BlockID block, ValueID condition, BlockID then, BlockID otherwise)
    {
        auto& terminator = at(block).terminator;
        terminator = Terminator { };
        terminator.kind = TerminatorKind::Branch;
        terminator.value = condition;
        terminator.targets.append(Target { then, { } });
        terminator.targets.append(Target { otherwise, { } });
    }

    unsigned useCountOf(ValueID id)
    {
        unsigned count = 0;
        auto look = [&](ValueID& child) {
            count += child == id;
        };
        for (auto& block : m_function.blocks) {
            for (ValueID user : block.values)
                at(user).forEachChild(look);
            block.terminator.forEachChild(look);
        }
        return count;
    }

    struct StepSite {
        ValueID call;
        ValueID generator;
        ValueID method;
        int64_t mode;
    };

    std::optional<StepSite> stepSiteAt(ValueID id)
    {
        static constexpr ASCIILiteral names[] = { "next"_s, "return"_s, "throw"_s };
        static_assert(!static_cast<int>(JSGenerator::ResumeMode::NormalMode) && static_cast<int>(JSGenerator::ResumeMode::ReturnMode) == 1 && static_cast<int>(JSGenerator::ResumeMode::ThrowMode) == 2);
        auto& call = at(id);
        if (call.arguments.size() > 1 || call.mustBeProperTailCall || call.isOnSlowPath || !m_program.at(call.type).anyObject)
            return std::nullopt;
        StepSite site { id, { }, { }, 0 };
        ValueID receiver;
        StringID name;
        if (call.opcode == Opcode::CallMethod) {
            receiver = call.child(0);
            name = call.string();
        } else if (call.opcode == Opcode::Call && call.child(1) && at(call.child(0)).opcode == Opcode::GetById) {
            site.method = call.child(0);
            receiver = at(site.method).child(0);
            name = at(site.method).string();
        } else
            return std::nullopt;
        if (isDeclaredGenerator(receiver))
            site.generator = at(receiver).arguments[0];
        else if (isCreatedGenerator(receiver))
            site.generator = receiver;
        else
            return std::nullopt;
        if (site.method && ((call.child(1) != receiver && call.child(1) != site.generator) || useCountOf(site.method) != 1))
            return std::nullopt;
        for (unsigned mode = 0; mode < std::size(names); ++mode) {
            if (auto known = m_program.find(asBytes(names[mode].span8())); known && *known == name) {
                site.mode = mode;
                return site;
            }
        }
        return std::nullopt;
    }

    unsigned stepDeclaredGeneratorsInPlace()
    {
        Vector<ValueID, 4> calls;
        for (auto& block : m_function.blocks) {
            for (ValueID id : block.values) {
                if (stepSiteAt(id))
                    calls.append(id);
            }
        }
        for (ValueID call : calls)
            stepInPlace(*stepSiteAt(call));
        return calls.size();
    }

    void stepInPlace(const StepSite& step)
    {
        ValueID isUnmodified;
        if (step.method) {
            Site site = siteOf(step.method);
            Value like = at(step.method);
            Cut cut = cutAt(site);
            BlockID reads = addBlock();
            BlockID isKnown = addBlock();
            BlockID found = addBlock();
            if (isCreatedGenerator(step.generator))
                isUnmodified = appendOperation(site.block, Operations::IsUnmodifiedGenerator, m_program.types.boolean, like, { step.generator, append(site.block, makeBoolean(true, like)) });
            else
                isUnmodified = appendOperation(site.block, Operations::IsUnmodifiedGenerator, m_program.types.boolean, like, { step.generator });
            branch(site.block, isUnmodified, isKnown, reads);
            ValueID method = addParameter(found, m_program.types.anything, like);
            replaceUses(step.method, method);
            at(step.method).children[0] = step.generator;
            at(reads).values.append(step.method);
            jump(reads, Target { found, { step.method } }, cut.handler);
            jump(isKnown, Target { found, { append(isKnown, make(Opcode::Undefined, m_program.types.undefined, like)) } });
            jump(found, cut.after);
        }
        static constexpr Generator::ResumeMode modes[] = { Generator::ResumeMode::Next, Generator::ResumeMode::Return, Generator::ResumeMode::Throw };
        Site site = siteOf(step.call);
        Value like = at(step.call);
        Cut cut = cutAt(site);
        BlockID steps = step.method ? addBlock() : site.block;
        BlockID asks = addBlock();
        BlockID wasStepped = addBlock();
        BlockID calls = addBlock();
        BlockID stepped = addBlock();
        if (step.method)
            branch(site.block, isUnmodified, steps, calls);

        ValueID result = addParameter(stepped, like.type, like);
        replaceUses(step.call, result);

        ValueID sent = like.arguments.isEmpty() ? append(steps, make(Opcode::Undefined, m_program.types.undefined, like)) : like.arguments[0];
        ValueID mode = appendInteger(steps, static_cast<int64_t>(modes[step.mode]), m_program.types.int32, like);
        ValueID check = appendInteger(steps, static_cast<int64_t>(step.method ? Generator::StepCheck::None : isCreatedGenerator(step.generator) ? Generator::StepCheck::IsUnmodified : Generator::StepCheck::IsUnmodifiedGenerator), m_program.types.int32, like);
        ValueID value = appendOperation(steps, Operations::StepGenerator, m_program.types.anything, like, { step.generator, mode, sent, check });
        jump(steps, Target { asks, { } }, cut.handler);

        ValueID status = appendOperation(asks, Operations::GeneratorStepStatus, m_program.types.int32, like, { value });
        ValueID wasNotStepped = appendCompare(asks, Opcode::StrictEq, status, appendInteger(asks, Generator::wasNotStepped, m_program.types.int32, like), like);
        BlockID otherwise = calls;
        if (step.method) {
            otherwise = addBlock();
            ValueID modeInEngine = appendInteger(otherwise, step.mode, m_program.types.int32, like);
            jump(otherwise, Target { stepped, { appendOperation(otherwise, Operations::ResumeGenerator, like.type, like, { step.generator, modeInEngine, sent }) } }, cut.handler);
        }
        branch(asks, wasNotStepped, otherwise, wasStepped);

        ValueID isDone = appendCompare(wasStepped, Opcode::StrictEq, status, appendInteger(wasStepped, Generator::didComplete, m_program.types.int32, like), like);
        jump(wasStepped, Target { stepped, { appendOperation(wasStepped, Operations::MakeIteratorResult, m_program.types.object, like, { value, isDone }) } });

        auto& call = at(step.call);
        call.isOnSlowPath = true;
        call.isInTailPosition = false;
        if (call.opcode == Opcode::CallMethod)
            call.children[0] = step.generator;
        at(calls).values.append(step.call);
        jump(calls, Target { stepped, { step.call } }, cut.handler);
        jump(stepped, cut.after);
    }

    struct ResultParts {
        ValueID object;
        ValueID value;
        ValueID isDone;
        ValueID isInParts;
    };

    unsigned keepIteratorResultsInParts()
    {
        auto done = m_program.find(asBytes("done"_s.span8()));
        auto value = m_program.find(asBytes("value"_s.span8()));
        if (!done || !value)
            return 0;
        unsigned count = m_function.values.size();
        BitVector isResult(count);
        Vector<ValueID, 8> results;
        auto add = [&](ValueID id) {
            if (!isResult.set(id.index()))
                results.append(id);
        };
        struct Literal {
            ValueID value;
            ValueID isDone;
            ValueID definesValue;
            ValueID definesDone;
            bool hasOtherProperties { false };
        };
        UncheckedKeyHashMap<uint32_t, Literal, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> literals;
        auto definedLiteral = [&](const Value& user) {
            bool defines = m_program.operationOf(user) == Operations::DefineField && user.arguments.size() == 3 && at(user.arguments[0]).opcode == Opcode::NewObject && user.arguments[2] != user.arguments[0];
            return defines ? user.arguments[0] : ValueID();
        };
        for (auto& block : m_function.blocks) {
            for (ValueID id : block.values) {
                auto& user = at(id);
                if (m_program.operationOf(user) == Operations::MakeIteratorResult)
                    add(id);
                ValueID object = definedLiteral(user);
                if (!object)
                    continue;
                auto& literal = literals.add(object.index(), Literal { }).iterator->value;
                auto& key = at(user.arguments[1]);
                if (key.opcode == Opcode::String && key.string() == *value && !literal.definesValue) {
                    literal.value = user.arguments[2];
                    literal.definesValue = id;
                } else if (key.opcode == Opcode::String && key.string() == *done && !literal.definesDone && m_program.at(at(user.arguments[2]).type).isBoolean()) {
                    literal.isDone = user.arguments[2];
                    literal.definesDone = id;
                } else
                    literal.hasOtherProperties = true;
            }
        }
        literals.removeIf([](auto& entry) {
            return entry.value.hasOtherProperties || !entry.value.definesValue || !entry.value.definesDone;
        });
        for (auto& block : m_function.blocks) {
            for (ValueID id : block.values) {
                if (at(id).opcode == Opcode::NewObject && literals.contains(id.index()))
                    add(id);
            }
        }
        if (results.isEmpty())
            return 0;
        Vector<uint32_t> group(count);
        for (unsigned i = 0; i < count; ++i)
            group[i] = i;
        auto groupOf = [&](ValueID id) {
            uint32_t index = id.index();
            while (group[index] != index)
                index = group[index] = group[group[index]];
            return index;
        };
        auto unite = [&](ValueID a, ValueID b) {
            group[groupOf(a)] = groupOf(b);
        };
        for (size_t known = 0; known != results.size();) {
            known = results.size();
            for (auto& block : m_function.blocks) {
                for (ValueID id : block.values) {
                    if (at(id).opcode == Opcode::Cast && isResult.get(at(id).child(0).index())) {
                        add(id);
                        unite(id, at(id).child(0));
                    }
                }
                for (auto& target : block.terminator.targets) {
                    for (unsigned i = 0; i < target.arguments.size(); ++i) {
                        if (!isResult.get(target.arguments[i].index()))
                            continue;
                        ValueID parameter = at(target.block).parameters[i];
                        add(parameter);
                        unite(parameter, target.arguments[i]);
                    }
                }
            }
        }
        BitVector escapes(count);
        BitVector mergesWithObjects(count);
        Vector<ValueID, 8> reads;
        Vector<ValueID, 8> checks;
        for (auto& block : m_function.blocks) {
            for (ValueID id : block.values) {
                auto& user = at(id);
                ValueID defined = definedLiteral(user);
                bool definesLiteral = defined && literals.contains(defined.index());
                user.forEachChild([&](ValueID& child) {
                    if (!isResult.get(child.index()) || (definesLiteral && &child == &user.arguments[0]))
                        return;
                    bool isOperand = &child == &user.children[0];
                    if (isOperand && user.opcode == Opcode::GetById && (user.string() == *done || user.string() == *value))
                        reads.append(id);
                    else if (m_program.operationOf(user) == Operations::RequireIteratorResultObject && !useCountOf(id))
                        checks.append(id);
                    else if (!isOperand || user.opcode != Opcode::Cast || !m_program.at(user.type).anyObject)
                        escapes.set(groupOf(child));
                });
            }
            if (block.terminator.value && isResult.get(block.terminator.value.index()))
                escapes.set(groupOf(block.terminator.value));
            for (auto& target : block.terminator.targets) {
                for (unsigned i = 0; i < target.arguments.size(); ++i) {
                    ValueID parameter = at(target.block).parameters[i];
                    if (isResult.get(parameter.index()) && !isResult.get(target.arguments[i].index()))
                        mergesWithObjects.set(groupOf(parameter));
                }
            }
        }
        results.removeAllMatching([&](ValueID id) {
            return escapes.get(groupOf(id));
        });
        if (results.isEmpty())
            return 0;
        reads.removeAllMatching([&](ValueID id) {
            return escapes.get(groupOf(at(id).child(0)));
        });
        checks.removeAllMatching([&](ValueID id) {
            return escapes.get(groupOf(at(id).arguments[0]));
        });

        UncheckedKeyHashMap<uint32_t, ResultParts, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> parts;
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            unsigned numberOfParameters = m_function.blocks[b].parameters.size();
            for (unsigned i = 0; i < numberOfParameters; ++i) {
                ValueID id = m_function.blocks[b].parameters[i];
                if (!isResult.get(id.index()) || escapes.get(groupOf(id)))
                    continue;
                Value like = at(id);
                at(id).type = m_program.types.anything;
                at(id).rep = repOf(m_program.at(m_program.types.anything));
                ResultParts held { id, addParameter(BlockID(b), m_program.types.anything, like), addParameter(BlockID(b), m_program.types.boolean, like), { } };
                if (mergesWithObjects.get(groupOf(id)))
                    held.isInParts = addParameter(BlockID(b), m_program.types.boolean, like);
                parts.add(id.index(), held);
            }
        }
        for (ValueID id : results) {
            Value like = at(id);
            if (like.opcode != Opcode::Parameter && like.opcode != Opcode::Cast) {
                Site site = siteOf(id);
                ResultParts held { id, { }, { }, { } };
                if (like.opcode == Opcode::NewObject) {
                    Literal literal = literals.get(id.index());
                    held.value = literal.value;
                    held.isDone = literal.isDone;
                    remove(literal.definesValue);
                    remove(literal.definesDone);
                } else {
                    held.value = like.arguments[0];
                    held.isDone = like.arguments[1];
                }
                if (mergesWithObjects.get(groupOf(id)))
                    held.isInParts = prepend(site.block, makeBoolean(true, like));
                parts.add(id.index(), held);
                at(id) = make(Opcode::Undefined, m_program.types.undefined, like);
            }
        }
        auto partsOf = [&](ValueID id) {
            while (at(id).opcode == Opcode::Cast)
                id = at(id).child(0);
            return parts.get(id.index());
        };
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            for (unsigned t = 0; t < m_function.blocks[b].terminator.targets.size(); ++t) {
                BlockID to = m_function.blocks[b].terminator.targets[t].block;
                unsigned passed = m_function.blocks[b].terminator.targets[t].arguments.size();
                for (unsigned i = 0; i < passed; ++i) {
                    ValueID parameter = at(to).parameters[i];
                    if (!parts.contains(parameter.index()) || at(parameter).opcode != Opcode::Parameter)
                        continue;
                    ValueID argument = m_function.blocks[b].terminator.targets[t].arguments[i];
                    bool isAlsoInParts = isResult.get(argument.index()) && !escapes.get(groupOf(argument));
                    ResultParts held;
                    if (isAlsoInParts)
                        held = partsOf(argument);
                    else {
                        Value like = at(argument);
                        held = { argument, prepend(BlockID(b), make(Opcode::Undefined, m_program.types.undefined, like)), prepend(BlockID(b), makeBoolean(false, like)), prepend(BlockID(b), makeBoolean(false, like)) };
                    }
                    auto& arguments = m_function.blocks[b].terminator.targets[t].arguments;
                    arguments[i] = held.object;
                    arguments.append(held.value);
                    arguments.append(held.isDone);
                    if (parts.get(parameter.index()).isInParts)
                        arguments.append(held.isInParts);
                }
            }
        }
        auto replaceRead = [&](ValueID read, ValueID with) {
            replaceUses(read, with);
            for (auto& held : parts.values()) {
                if (held.value == read)
                    held.value = with;
            }
        };
        for (ValueID read : reads) {
            ResultParts held = partsOf(at(read).child(0));
            Value like = at(read);
            bool readsDone = like.string() == *done;
            auto claim = [&] {
                Value cast = make(Opcode::Cast, like.type, like);
                cast.children[0] = held.value;
                cast.immediates[1] = static_cast<uint32_t>(CastReason::Claim);
                cast.copyPredictionFrom(like);
                return cast;
            };
            if (!held.isInParts) {
                if (readsDone) {
                    replaceRead(read, held.isDone);
                    remove(read);
                } else
                    at(read) = claim();
                continue;
            }
            Site site = siteOf(read);
            Cut cut = cutAt(site);
            BlockID isHeldInParts = addBlock();
            BlockID isObject = addBlock();
            BlockID wasRead = addBlock();
            branch(site.block, held.isInParts, isHeldInParts, isObject);
            ValueID merged = addParameter(wasRead, like.type, like);
            at(merged).copyPredictionFrom(like);
            replaceRead(read, merged);
            jump(isHeldInParts, Target { wasRead, { readsDone ? held.isDone : append(isHeldInParts, claim()) } });
            at(read).children[0] = held.object;
            at(isObject).values.append(read);
            jump(isObject, Target { wasRead, { read } }, cut.handler);
            jump(wasRead, cut.after);
        }
        for (ValueID check : checks) {
            ResultParts held = partsOf(at(check).arguments[0]);
            if (!held.isInParts) {
                remove(check);
                continue;
            }
            Site site = siteOf(check);
            Cut cut = cutAt(site);
            BlockID isObject = addBlock();
            BlockID wasChecked = addBlock();
            branch(site.block, held.isInParts, wasChecked, isObject);
            at(check).arguments[0] = held.object;
            at(isObject).values.append(check);
            jump(isObject, Target { wasChecked, { } }, cut.handler);
            jump(wasChecked, cut.after);
        }
        for (ValueID id : results) {
            if (at(id).opcode == Opcode::Cast)
                remove(id);
        }
        return results.size();
    }

    const Program& m_program;
    Function& m_function;
    ValueID m_frame;
    LayoutID m_layout;
    FunctionID m_resume;
    uint32_t m_inliningDepth { Format::none };
};

} // anonymous namespace

void internNamesForGeneratorLoopFusion(Program& program)
{
    if (program.usesOperation(Operations::AppendSpread))
        program.intern(asBytes("push"_s.span8()));
}

unsigned fuseGeneratorLoops(const Program& program, Function& function)
{
    return GeneratorLoopFusion(program, function).run();
}

unsigned stepGeneratorsInPlace(const Program& program, Function& function)
{
    return GeneratorLoopFusion(program, function).stepWhatIsNotFused();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
