/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedPasses.h"

#if ENABLE(AOT)

namespace JSC { namespace AOT { namespace Typed {

namespace {

// Puts the body of a function in the place of a call of it. What is copied is the body as the program has it, whatever has become of
// that function's own copy by now: so the outcome does not depend on which function is worked on first. The calls that come along are
// looked at in turn, down to a depth. A call that closes a cycle stays a call.
class Inlining {
public:
    Inlining(const Program& program, FunctionID id, Function& caller)
        : m_program(program)
        , m_id(id)
        , m_caller(caller)
    {
    }

    unsigned run()
    {
        unsigned replaced = 0;
        unsigned size = sizeOf(m_caller, UINT_MAX);
        m_copiedFrom.fill(none, m_caller.blocks.size());
        Vector<BlockID> worklist;
        for (unsigned b = m_caller.blocks.size(); b--;)
            worklist.append(BlockID(b));
        while (!worklist.isEmpty()) {
            BlockID block = worklist.takeLast();
            for (unsigned k = 0; k < m_caller.at(block).values.size(); ++k) {
                ValueID call = m_caller.at(block).values[k];
                if (m_caller.at(call).opcode != Opcode::CallDirect || depthOf(m_caller.at(call)) >= maxDepth)
                    continue;
                FunctionID callee = m_caller.at(call).function();
                if (closesCycle(callee, block)) {
                    m_caller.at(call).immediates[1] = maxDepth;
                    continue;
                }
                if (!mayBeCopied(m_program.at(callee), m_caller.at(call)))
                    continue;
                unsigned growsBy = sizeOf(m_program.at(callee), small + 1);
                if (growsBy > small || size + growsBy > maxGrownTo)
                    continue;
                unsigned first = m_caller.blocks.size();
                copyIn(block, k, callee);
                for (unsigned b = m_caller.blocks.size(); b-- > first;)
                    worklist.append(BlockID(b));
                size += growsBy;
                ++replaced;
                break;
            }
        }
        if (replaced)
            cleanUp(m_program, m_caller);
        return replaced;
    }

private:
    // A function this small is copied to every call.
    static constexpr unsigned small = 40;
    // A function this large takes in no more.
    static constexpr unsigned maxGrownTo = 4000;
    static constexpr uint32_t none = UINT32_MAX;
    // A call that came along with a body that came along with a body, and so on this many times, stays a call.
    static constexpr uint32_t maxDepth = 4;

    // How a call got here is forgotten from one round to the next, and a call by name becomes a direct call in between. So the depth
    // is kept in the second immediate of CallDirect, CallMethod and Call, which nothing else uses. resolveCalls() hands it on.
    static uint32_t depthOf(const Value& call) { return call.immediates[1] == Format::none ? 0 : call.immediates[1]; }

    // A function that was copied in, and what the call of it was copied in with.
    struct Copy {
        FunctionID function;
        uint32_t outer;
    };

    // No further than the limit.
    static unsigned sizeOf(const Function& function, unsigned limit)
    {
        unsigned size = 0;
        for (auto& block : function.blocks) {
            for (ValueID id : block.values) {
                switch (function.at(id).opcode) {
                case Opcode::Undefined:
                case Opcode::Null:
                case Opcode::Boolean:
                case Opcode::Integer:
                case Opcode::Double:
                case Opcode::String:
                case Opcode::Cast:
                case Opcode::LoopHint:
                    break;
                default:
                    if (++size >= limit)
                        return size;
                }
            }
        }
        return size;
    }

    bool closesCycle(FunctionID callee, BlockID block) const
    {
        if (callee == m_id)
            return true;
        for (uint32_t at = m_copiedFrom[block.index()]; at != none; at = m_copies[at].outer) {
            if (m_copies[at].function == callee)
                return true;
        }
        return false;
    }

    TypeID resultTypeOf(const Function& callee) const { return callee.resultType ? callee.resultType : m_program.types.anything; }

    bool mayBeCopied(const Function& callee, const Value& call) const
    {
        if (callee.blocks.isEmpty() || callee.has(Format::FunctionIsAsync | Format::FunctionIsGenerator | FunctionIsGenericEntrypoint | FunctionUsesArgumentBuffer))
            return false;
        // What is not written in TypeScript is compiled without regard to types, and the function says which it is.
        if (callee.isTyped() != m_caller.isTyped())
            return false;
        if (call.arguments.size() != callee.blocks[0].parameters.size())
            return false;
        auto& result = m_program.at(resultTypeOf(callee));
        if (!result.isWithin(m_program.at(call.type), m_program))
            return false;
        bool yieldsUndefined = m_program.at(m_program.types.undefined).isWithin(result, m_program);
        for (auto& block : callee.blocks) {
            if (block.terminator.kind == TerminatorKind::Return && !block.terminator.value && !yieldsUndefined)
                return false;
        }
        return true;
    }

    BlockID addBlock(uint32_t copiedFrom)
    {
        m_caller.blocks.append(Block { });
        m_copiedFrom.append(copiedFrom);
        return BlockID(m_caller.blocks.size() - 1);
    }

    ValueID addValue(Value&& value)
    {
        m_caller.values.append(WTF::move(value));
        return ValueID(m_caller.values.size() - 1);
    }

    // Replaces the value at `k` of the block, a direct call, with the body of the function.
    void copyIn(BlockID block, unsigned k, FunctionID calleeID)
    {
        auto& callee = m_program.at(calleeID);
        ValueID call = m_caller.at(block).values[k];
        Arguments arguments = m_caller.at(call).arguments;
        uint32_t position = m_caller.at(call).position;
        uint32_t depth = depthOf(m_caller.at(call)) + 1;
        bool isWatched = m_caller.at(block).terminator.kind == TerminatorKind::Invoke && k + 1 == m_caller.at(block).values.size();

        BlockID after = addBlock(m_copiedFrom[block.index()]);
        {
            auto& from = m_caller.at(block);
            m_caller.at(after).values.append(from.values.subspan(k + 1));
            from.values.shrink(k);
            m_caller.at(after).terminator = std::exchange(from.terminator, Terminator { });
        }
        Target handler;
        if (isWatched) {
            auto& invoke = m_caller.at(after).terminator;
            handler = invoke.targets[1];
            invoke.kind = TerminatorKind::Jump;
            invoke.targets.shrink(1);
        }
        // What the call yielded is what the function is declared to yield.
        {
            Value result;
            result.opcode = Opcode::Parameter;
            result.position = position;
            result.type = resultTypeOf(callee);
            result.takePredictionOf(m_caller.at(call));
            result.rep = repOf(m_program.at(result.type));
            result.immediates = { after.index(), 0 };
            m_caller.at(call) = WTF::move(result);
            m_caller.at(after).parameters.append(call);
        }

        m_copies.append(Copy { calleeID, m_copiedFrom[block.index()] });
        uint32_t copiedFrom = m_copies.size() - 1;
        unsigned first = m_caller.blocks.size();
        for (unsigned i = 0; i < callee.blocks.size(); ++i)
            addBlock(copiedFrom);

        Vector<ValueID> map(callee.values.size());
        for (unsigned i = 0; i < arguments.size(); ++i) {
            ValueID parameter = callee.blocks[0].parameters[i];
            ValueID argument = arguments[i];
            // The body was written, and held to its types, for a value of the type of the parameter. Whether it may as well go by the type
            // of the argument, which says more, is for simplifyCasts() to find out.
            TypeID type = callee.at(parameter).type;
            if (type != m_caller.at(argument).type || callee.at(parameter).hasPredictionUnlike(m_caller.at(argument))) {
                Value upcast;
                upcast.opcode = Opcode::Cast;
                upcast.position = position;
                upcast.type = type;
                upcast.takePredictionOf(callee.at(parameter));
                upcast.rep = callee.at(parameter).rep;
                upcast.children[0] = argument;
                upcast.immediates[1] = static_cast<uint32_t>(CastReason::Upcast);
                argument = addValue(WTF::move(upcast));
                m_caller.at(block).values.append(argument);
            }
            map[parameter.index()] = argument;
        }
        auto copy = [&](ValueID value) {
            map[value.index()] = addValue(Value(callee.at(value)));
        };
        for (unsigned i = 0; i < callee.blocks.size(); ++i) {
            if (i) {
                for (ValueID parameter : callee.blocks[i].parameters)
                    copy(parameter);
            }
            for (ValueID value : callee.blocks[i].values)
                copy(value);
        }
        auto mapped = [&](ValueID& value) {
            value = map[value.index()];
        };
        for (unsigned i = 0; i < callee.blocks.size(); ++i) {
            BlockID made(first + i);
            if (i) {
                for (ValueID parameter : callee.blocks[i].parameters) {
                    m_caller.at(map[parameter.index()]).immediates[0] = made.index();
                    m_caller.at(made).parameters.append(map[parameter.index()]);
                }
            }
            for (ValueID value : callee.blocks[i].values) {
                auto& copied = m_caller.at(map[value.index()]);
                copied.forEachChild(mapped);
                if (copied.opcode == Opcode::NewObjectWithShape)
                    copied.immediates[1] = m_caller.allocationOf(Allocation(callee.allocations[copied.allocation()]));
                else if (copied.opcode == Opcode::CallDirect || copied.opcode == Opcode::CallMethod || copied.opcode == Opcode::Call)
                    copied.immediates[1] = depth;
                m_caller.at(made).values.append(map[value.index()]);
            }
            Terminator terminator = callee.blocks[i].terminator;
            if (terminator.kind == TerminatorKind::Return) {
                ValueID result;
                if (terminator.value)
                    result = map[terminator.value.index()];
                else {
                    Value undefined;
                    undefined.position = position;
                    undefined.type = m_program.types.undefined;
                    undefined.rep = repOf(m_program.at(undefined.type));
                    result = addValue(WTF::move(undefined));
                    m_caller.at(made).values.append(result);
                }
                terminator = Terminator { };
                terminator.kind = TerminatorKind::Jump;
                terminator.targets.append(Target { after, Arguments { result } });
            } else {
                terminator.forEachChild(mapped);
                for (auto& target : terminator.targets)
                    target.block = BlockID(first + target.block.index());
            }
            m_caller.at(made).terminator = WTF::move(terminator);
        }
        auto& jump = m_caller.at(block).terminator;
        jump.kind = TerminatorKind::Jump;
        jump.targets.append(Target { BlockID(first), { } });
        if (isWatched)
            watch(first, first + callee.blocks.size(), copiedFrom, handler);
    }

    // What was copied in the place of a call that a handler watches is watched by it: whatever may throw ends its block, and what is
    // thrown and not caught goes there.
    void watch(unsigned begin, unsigned end, uint32_t copiedFrom, const Target& handler)
    {
        for (unsigned b = begin; b < end; ++b) {
            BlockID block(b);
            if (m_caller.at(block).terminator.kind == TerminatorKind::Throw && m_caller.at(block).terminator.targets.isEmpty())
                m_caller.at(block).terminator.targets.append(handler);
            for (unsigned i = 0; i < m_caller.at(block).values.size(); ++i) {
                // Its last value is watched by a handler of what was copied.
                if (i + 1 == m_caller.at(block).values.size() && m_caller.at(block).terminator.kind == TerminatorKind::Invoke)
                    break;
                // A store to an object nobody has seen stays with the allocation.
                if (!mayThrow(m_program, m_caller, m_caller.at(m_caller.at(block).values[i])) || isInitializingStore(m_program, m_caller, m_caller.at(block), i))
                    continue;
                BlockID rest = addBlock(copiedFrom);
                auto& from = m_caller.at(block);
                m_caller.at(rest).values.append(from.values.subspan(i + 1));
                from.values.shrink(i + 1);
                m_caller.at(rest).terminator = std::exchange(from.terminator, Terminator { });
                from.terminator.kind = TerminatorKind::Invoke;
                from.terminator.targets.append(Target { rest, { } });
                from.terminator.targets.append(handler);
                block = rest;
                i = UINT_MAX;
            }
        }
    }

    const Program& m_program;
    FunctionID m_id;
    Function& m_caller;
    Vector<Copy> m_copies;
    // By block: the copy it is part of, among m_copies.
    Vector<uint32_t> m_copiedFrom;
};

} // anonymous namespace

unsigned inlineCalls(const Program& program, FunctionID id, Function& function)
{
    return Inlining(program, id, function).run();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
