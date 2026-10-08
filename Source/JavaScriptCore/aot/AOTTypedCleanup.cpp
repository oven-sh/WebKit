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

// Takes out what carries no information: reads of what can only be one value, branches on a constant, blocks nothing reaches, parameters
// that are always the same value, jumps to a block that has no other way in, values nobody uses that do nothing.
//
// A parameter is of its own type, which may say less than the type of the one value it is given. Where the two differ the parameter
// becomes an Upcast of that value. Whether its users may as well look at the value itself is for simplifyCasts(), which can compare types.
// Likewise where something is expected of the parameter that is not expected of the value: see Value::prediction.
class Cleanup {
public:
    Cleanup(const Program& program, Function& function)
        : m_program(program)
        , m_function(function)
    {
    }

    void run()
    {
        if (m_function.blocks.isEmpty())
            return;
        foldReads();
        bool changed;
        do {
            foldBranches();
            dropUnreachedBlocks();
            changed = removeTrivialParameters();
            changed |= mergeChains();
            changed |= removeUnusedValues();
        } while (changed);
    }

private:
    // A read that cannot throw, and whose type has one value, is that value.
    void foldReads()
    {
        for (auto& block : m_function.blocks) {
            for (ValueID id : block.values) {
                auto& value = m_function.at(id);
                if ((value.opcode != Opcode::GetModuleVariable && value.opcode != Opcode::GetField) || mayThrow(m_program, m_function, value))
                    continue;
                // What a handler watches stays what it is.
                if (block.terminator.kind == TerminatorKind::Invoke && id == block.values.last())
                    continue;
                auto& type = m_program.at(value.type);
                if (type.hasObjects())
                    continue;
                Value constant = value;
                constant.children = { };
                constant.immediates = { Format::none, Format::none };
                switch (type.primitives) {
                case Primitive::Undefined:
                    constant.opcode = Opcode::Undefined;
                    break;
                case Primitive::Null:
                    constant.opcode = Opcode::Null;
                    break;
                case Primitive::False:
                case Primitive::True:
                    constant.opcode = Opcode::Boolean;
                    constant.immediates[0] = type.primitives == Primitive::True;
                    break;
                case Primitive::Int:
                    if (type.integers.least != type.integers.most)
                        continue;
                    constant.opcode = Opcode::Integer;
                    constant.setBits(static_cast<uint64_t>(type.integers.least));
                    break;
                default:
                    continue;
                }
                value = WTF::move(constant);
            }
        }
    }

    void foldBranches()
    {
        for (auto& block : m_function.blocks) {
            auto& terminator = block.terminator;
            if (terminator.kind != TerminatorKind::Branch || m_function.at(terminator.value).opcode != Opcode::Boolean)
                continue;
            Target taken = WTF::move(terminator.targets[m_function.at(terminator.value).boolean() ? 0 : 1]);
            terminator = Terminator { };
            terminator.kind = TerminatorKind::Jump;
            terminator.targets.append(WTF::move(taken));
        }
    }

    void dropUnreachedBlocks()
    {
        BitVector seen(m_function.blocks.size());
        seen.set(0);
        Vector<BlockID> worklist { BlockID(0u) };
        while (!worklist.isEmpty()) {
            for (auto& target : m_function.at(worklist.takeLast()).terminator.targets) {
                if (!seen.set(target.block.index()))
                    worklist.append(target.block);
            }
        }
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            if (!seen.get(b))
                m_function.blocks[b] = Block { };
        }
    }

    void resetReplacements()
    {
        m_replaced.resize(m_function.values.size());
        for (unsigned i = 0; i < m_replaced.size(); ++i)
            m_replaced[i] = ValueID(i);
    }

    ValueID resolve(ValueID value) const
    {
        if (!value)
            return value;
        while (m_replaced[value.index()] != value)
            value = m_replaced[value.index()];
        return value;
    }

    void rewrite()
    {
        auto replace = [&](ValueID& value) {
            value = resolve(value);
        };
        for (auto& value : m_function.values)
            value.forEachChild(replace);
        for (auto& block : m_function.blocks)
            block.terminator.forEachChild(replace);
    }

    bool standsFor(ValueID value, ValueID parameter) const
    {
        return m_function.at(value).type == m_function.at(parameter).type && !m_function.at(parameter).hasPredictionUnlike(m_function.at(value));
    }

    // The parameter keeps its number, its type, its representation and what is expected of it.
    void makeUpcast(ValueID parameter, ValueID of)
    {
        auto& value = m_function.at(parameter);
        value.opcode = Opcode::Cast;
        value.children = { of };
        value.immediates = { Format::none, static_cast<uint32_t>(CastReason::Upcast) };
    }

    // A parameter that is only ever given itself and one other value is that value.
    bool removeTrivialParameters()
    {
        resetReplacements();
        Vector<Vector<Vector<ValueID, 2>>> given;
        for (auto& block : m_function.blocks)
            given.append(Vector<Vector<ValueID, 2>>(block.parameters.size()));
        for (auto& block : m_function.blocks) {
            for (auto& target : block.terminator.targets) {
                for (unsigned i = 0; i < target.arguments.size(); ++i)
                    given[target.block.index()][i].append(target.arguments[i]);
            }
        }
        bool any = false;
        bool changed;
        do {
            changed = false;
            for (unsigned b = 1; b < m_function.blocks.size(); ++b) {
                auto& parameters = m_function.blocks[b].parameters;
                for (unsigned i = 0; i < parameters.size(); ++i) {
                    ValueID parameter = parameters[i];
                    if (m_replaced[parameter.index()] != parameter)
                        continue;
                    ValueID only;
                    bool isTrivial = true;
                    for (ValueID argument : given[b][i]) {
                        argument = resolve(argument);
                        if (argument == parameter || argument == only)
                            continue;
                        if (only) {
                            isTrivial = false;
                            break;
                        }
                        only = argument;
                    }
                    if (isTrivial && only) {
                        m_replaced[parameter.index()] = only;
                        changed = true;
                    }
                }
            }
            any |= changed;
        } while (changed);
        if (!any)
            return false;

        BitVector isRemoved(m_function.values.size());
        for (unsigned v = 0; v < m_replaced.size(); ++v) {
            if (m_replaced[v] != ValueID(v))
                isRemoved.set(v);
        }
        for (auto& block : m_function.blocks) {
            for (auto& target : block.terminator.targets) {
                auto& parameters = m_function.at(target.block).parameters;
                Arguments kept;
                for (unsigned i = 0; i < target.arguments.size(); ++i) {
                    if (!isRemoved.get(parameters[i].index()))
                        kept.append(target.arguments[i]);
                }
                target.arguments = WTF::move(kept);
            }
        }
        // All of them are followed to the value they are before any of them becomes a value of its own.
        Vector<ValueID> values(m_replaced.size());
        for (unsigned v : isRemoved)
            values[v] = resolve(ValueID(v));
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            auto& block = m_function.blocks[b];
            // What was caught comes first in a handler.
            unsigned at = !block.values.isEmpty() && m_function.at(block.values[0]).opcode == Opcode::CatchValue ? 1 : 0;
            for (ValueID parameter : block.parameters) {
                if (!isRemoved.get(parameter.index()))
                    continue;
                ValueID value = values[parameter.index()];
                m_replaced[parameter.index()] = value;
                if (standsFor(value, parameter))
                    continue;
                m_replaced[parameter.index()] = parameter;
                makeUpcast(parameter, value);
                block.values.insert(at++, parameter);
            }
            block.parameters.removeAllMatching([&](ValueID parameter) {
                return isRemoved.get(parameter.index());
            });
            for (unsigned i = 0; i < block.parameters.size(); ++i)
                m_function.at(block.parameters[i]).immediates = { b, i };
        }
        rewrite();
        return true;
    }

    // A block that ends in a jump to a block with no other way in takes that block in.
    bool mergeChains()
    {
        Vector<unsigned> waysIn;
        waysIn.fill(0, m_function.blocks.size());
        waysIn[0] = 1;
        for (auto& block : m_function.blocks) {
            for (auto& target : block.terminator.targets)
                ++waysIn[target.block.index()];
        }
        resetReplacements();
        bool any = false;
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            for (;;) {
                auto& terminator = m_function.blocks[b].terminator;
                if (terminator.kind != TerminatorKind::Jump)
                    break;
                unsigned next = terminator.targets[0].block.index();
                if (next == b || waysIn[next] != 1)
                    break;
                Arguments arguments = terminator.targets[0].arguments;
                Block taken = std::exchange(m_function.blocks[next], Block { });
                for (unsigned i = 0; i < taken.parameters.size(); ++i) {
                    ValueID parameter = taken.parameters[i];
                    if (standsFor(arguments[i], parameter)) {
                        m_replaced[parameter.index()] = arguments[i];
                        continue;
                    }
                    makeUpcast(parameter, arguments[i]);
                    m_function.blocks[b].values.append(parameter);
                }
                m_function.blocks[b].values.appendVector(taken.values);
                m_function.blocks[b].terminator = WTF::move(taken.terminator);
                waysIn[next] = 0;
                any = true;
            }
        }
        if (any)
            rewrite();
        return any;
    }

    // Whatever its operands are, carrying it out cannot be told from not carrying it out.
    bool doesNothing(const Value& value) const
    {
        switch (value.opcode) {
        case Opcode::GetModuleVariable:
        case Opcode::GetField:
            return !mayThrow(m_program, m_function, value);
        case Opcode::Undefined:
        case Opcode::Null:
        case Opcode::Boolean:
        case Opcode::Integer:
        case Opcode::Double:
        case Opcode::String:
        case Opcode::BigInt:
        case Opcode::FunctionReference:
        case Opcode::NewClosure:
        case Opcode::NewObjectWithShape:
        case Opcode::GetScope:
        case Opcode::LogicalNot:
        case Opcode::IsUndefinedOrNull:
        case Opcode::HasShapeOf:
        case Opcode::IsClosureOf:
            return true;
        case Opcode::Cast:
            return value.castReason() == CastReason::Upcast;
        default:
            return false;
        }
    }

    bool removeUnusedValues()
    {
        Vector<unsigned> uses;
        uses.fill(0, m_function.values.size());
        auto use = [&](ValueID value) {
            ++uses[value.index()];
        };
        for (auto& block : m_function.blocks) {
            for (ValueID id : block.values)
                std::as_const(m_function.at(id)).forEachChild(use);
            block.terminator.forEachChild([&](ValueID& value) {
                use(value);
            });
        }
        bool any = false;
        for (auto& block : m_function.blocks) {
            // The last value of a block that a handler watches is what may throw.
            unsigned end = block.values.size();
            if (end && block.terminator.kind == TerminatorKind::Invoke)
                --end;
            bool anyHere = false;
            for (unsigned i = end; i--;) {
                ValueID id = block.values[i];
                if (uses[id.index()] || !doesNothing(m_function.at(id)))
                    continue;
                std::as_const(m_function.at(id)).forEachChild([&](ValueID child) {
                    --uses[child.index()];
                });
                block.values[i] = ValueID();
                anyHere = true;
            }
            if (!anyHere)
                continue;
            block.values.removeAllMatching([](ValueID id) {
                return !id;
            });
            any = true;
        }
        return any;
    }

    const Program& m_program;
    Function& m_function;
    Vector<ValueID> m_replaced;
};

} // anonymous namespace

void cleanUp(const Program& program, Function& function)
{
    Cleanup(program, function).run();
}

bool mayThrow(const Program& program, const Function& function, const Value& value)
{
    switch (value.opcode) {
    case Opcode::Parameter:
    case Opcode::Undefined:
    case Opcode::Null:
    case Opcode::Boolean:
    case Opcode::Integer:
    case Opcode::Double:
    case Opcode::String:
    case Opcode::BigInt:
    case Opcode::FunctionReference:
    case Opcode::NewClosure:
    case Opcode::GetScope:
    case Opcode::GetArgument:
    case Opcode::CreateRest:
    case Opcode::PutModuleVariable:
    case Opcode::NewObjectWithShape:
    case Opcode::NewObject:
    // A value that is known to be of the type of the field goes to an environment, or to an object nobody has seen yet.
    case Opcode::PutField:
    case Opcode::StrictEq:
    case Opcode::LogicalNot:
    case Opcode::ToBoolean:
    case Opcode::TypeOf:
    case Opcode::IsUndefinedOrNull:
    case Opcode::HasShapeOf:
    case Opcode::IsClosureOf:
    case Opcode::CatchValue:
    case Opcode::LoopHint:
    case Opcode::GetResumeMode:
        return false;
    case Opcode::IsInstanceOfClass:
        return mayWalkPrototypeChain(program, function, value);
    // A variable holds nothing before its declaration has run, and to read it then throws. What it holds is held to the type of the read.
    case Opcode::GetModuleVariable: {
        auto& variable = program.variables[value.variable().index()];
        return variable.mayBeSeenUninitialized() || !program.at(variable.type ? variable.type : program.types.anything).isWithin(program.at(value.type), program);
    }
    // What the table knows is there, and reading it runs no code.
    case Opcode::GetGlobal:
    case Opcode::GetGlobalForTypeOf: {
        BuiltinName name = program.builtinName(value.string());
        return !Builtins::singleton().isGlobal(name) || program.isExtended(name);
    }
    case Opcode::GetField: {
        auto& layout = program.at(value.layout());
        if (isObjectLayout(layout.kind))
            return true;
        auto& field = layout.fields[value.field()];
        if (layout.kind == LayoutKind::Environment && !layout.isCoroutineFrame && !(field.flags & (fieldIsImmutable | fieldIsNeverSeenUninitialized)))
            return true;
        return !program.at(field.type ? field.type : program.types.anything).isWithin(program.at(value.type), program);
    }
    case Opcode::Cast: {
        if (value.castReason() == CastReason::Upcast)
            return false;
        auto& operand = program.typeOf(function, value.child(0));
        // A conversion may throw, and so may whatever goes through a plain object, for want of room.
        if (value.castReason() == CastReason::Coerce)
            return !operand.isWithin(program.at(value.type), program) || coercionMayChangeObjects(program, operand, program.at(value.coercedType()));
        if (value.castReason() == CastReason::NonNull && operand.mayBeUndefinedOrNull())
            return true;
        // With the name of a property it stands for the access, which turns away `undefined` and `null` whatever the type says.
        if (value.immediates[0] != Format::none)
            return operand.mayBeUndefinedOrNull();
        // What is of the type is not tested.
        return !program.isHeldAlready(operand, program.at(value.type), value.castReason());
    }
    case Opcode::Add:
    case Opcode::Sub:
    case Opcode::Mul:
    case Opcode::Div:
    case Opcode::Mod:
    case Opcode::Pow:
    case Opcode::BitAnd:
    case Opcode::BitOr:
    case Opcode::BitXor:
    case Opcode::LShift:
    case Opcode::RShift:
    case Opcode::URShift:
    case Opcode::Negate:
    case Opcode::ToNumber:
    case Opcode::ToNumeric:
    case Opcode::BitNot:
    case Opcode::Less:
    case Opcode::LessEq:
    case Opcode::Greater:
    case Opcode::GreaterEq:
    case Opcode::Eq:
        return !isNumericOperand(program.typeOf(function, value.child(0))) || (value.child(1) && !isNumericOperand(program.typeOf(function, value.child(1))));
    // Among these every access by name: whether what a field holds is of the type of the read is known once fields are placed.
    default:
        return true;
    }
}

// The iterator of an array has no `return` method, and the prototypes of the engine stay as they are: closing it does nothing.
static bool closesIteratorOfArray(const Program& program, const Function& function, const Value& value)
{
    return value.opcode == Opcode::IteratorClose && keyedAccessOf(program, function, value) == KeyedAccess::Array;
}

unsigned removeRedundantCatchBlocks(const Program& program, Function& function)
{
    unsigned removed = 0;
    for (unsigned b = 0; b < function.blocks.size(); ++b) {
        auto& handler = function.blocks[b];
        if (handler.terminator.kind != TerminatorKind::Throw || handler.values.isEmpty() || !handler.parameters.isEmpty())
            continue;
        ValueID caught = handler.values[0];
        if (function.at(caught).opcode != Opcode::CatchValue || handler.terminator.value != caught)
            continue;
        bool onlyRethrows = true;
        for (unsigned i = 1; i < handler.values.size(); ++i)
            onlyRethrows &= closesIteratorOfArray(program, function, function.at(handler.values[i]));
        // What unwinds to it unwinds to where it rethrows to.
        Vector<Target, 2> outer = handler.terminator.targets;
        for (auto& target : outer) {
            for (ValueID argument : target.arguments)
                onlyRethrows &= !handler.values.contains(argument);
        }
        if (!onlyRethrows)
            continue;
        bool isUnwoundTo = false;
        for (unsigned other = 0; other < function.blocks.size(); ++other) {
            if (other == b)
                continue;
            auto& terminator = function.blocks[other].terminator;
            switch (terminator.kind) {
            case TerminatorKind::Invoke:
                if (terminator.targets[1].block != BlockID(b))
                    break;
                if (outer.isEmpty()) {
                    terminator.kind = TerminatorKind::Jump;
                    terminator.targets.shrink(1);
                } else
                    terminator.targets[1] = outer[0];
                isUnwoundTo = true;
                break;
            case TerminatorKind::Throw:
                if (terminator.targets.isEmpty() || terminator.targets[0].block != BlockID(b))
                    break;
                terminator.targets = outer;
                isUnwoundTo = true;
                break;
            default:
                break;
            }
        }
        removed += isUnwoundTo;
    }
    return removed;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
