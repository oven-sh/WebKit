/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedPasses.h"

#if ENABLE(AOT)

#include "Options.h"
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

std::optional<LanguageOperation> languageOperationOf(const Program& program, const Value& value)
{
    auto& builtins = Builtins::singleton();
    BuiltinID entry;
    if (value.opcode == Opcode::Operation)
        entry = builtins.find(BuiltinKind::Operation, { }, program.builtinName(value.string()));
    else if (value.opcode == Opcode::CallBuiltin)
        entry = value.builtin();
    if (!entry || builtins.at(entry).implementation.kind != BuiltinImplementation::Operation)
        return std::nullopt;
    return builtins.at(entry).implementation.operation;
}

void collectPropertiesGivenTo(const Program& program, const Function& function, ValueID object, unsigned depth, Vector<StringID, 16>& names, unsigned& numberOfKeys)
{
    auto isObject = [&](ValueID id) {
        while (function.at(id).opcode == Opcode::Cast && function.at(id).castReason() == CastReason::Upcast)
            id = function.at(id).child(0);
        return id == object;
    };
    auto isExpectedToBeSymbol = [&](ValueID id) {
        TypeID prediction = function.at(id).prediction;
        auto& type = prediction ? program.at(prediction) : program.typeOf(function, id);
        return type.primitives & Primitive::Symbol && type.hasOnlyPrimitives(Primitive::Symbol | Primitive::UndefinedOrNull);
    };
    for (auto& block : function.blocks) {
        for (ValueID userID : block.values) {
            auto& user = function.at(userID);
            std::optional<StringID> name;
            if (user.opcode == Opcode::PutById && isObject(user.child(0)))
                name = user.string();
            else if (user.opcode == Opcode::PutByVal && isObject(user.child(0)))
                numberOfKeys += isExpectedToBeSymbol(user.child(1));
            else if (user.opcode == Opcode::CallBuiltin && languageOperationOf(program, user) == LanguageOperation::DefineField && isObject(user.arguments[0]) && function.at(user.arguments[1]).opcode == Opcode::String)
                name = function.at(user.arguments[1]).string();
            else if (user.opcode == Opcode::CallDirect && depth) {
                // Handed on, for `this` or as an argument: the arguments stand in the order of the parameters.
                auto& callee = program.at(user.function());
                if (callee.blocks.isEmpty())
                    continue;
                auto& parameters = callee.blocks[0].parameters;
                for (unsigned i = callee.captureTypes.size(); i < std::min<size_t>(user.arguments.size(), parameters.size()); ++i) {
                    if (isObject(user.arguments[i]))
                        collectPropertiesGivenTo(program, callee, parameters[i], depth - 1, names, numberOfKeys);
                }
            }
            if (name && !names.contains(*name))
                names.append(*name);
        }
    }
}

namespace {

// Escape analysis for array literals, for the array of a rest parameter where the call was inlined, and for `arguments` objects.
//
// An allocation that is only read (`.length`, `[index]`, an array pattern, the list of a spread call or of `apply`) is not made: each read is replaced by
// what it would find. A use that lets the allocation escape gets a copy of the allocation right in front of it, provided that nothing
// looks at the allocation afterwards: nobody can tell when an object was made that nobody has seen. That is how the slow path behind the
// guard of `f.apply(thisArg, arguments)` keeps its `arguments` object and the fast path has none.
//
// The array of a rest parameter where the arguments are in memory (CreateRest, in a general entry that has taken the body of its function:
// see inlineCalls()) is read from there. A spread call or `apply` passes the arguments on from there. What else needs an array and does not
// keep it gets one of its own.
//
// A closure is never read, so all there is to do for it is the second half: it is made in front of each use that nothing follows. The callback of
// `a.map(x => ..)` is inlined into the loops that stand for `map`, and only what is done for an array that is none of the usual needs the closure.
//
// All of this looks at one function.
class ArrayEscapeAnalysis {
public:
    ArrayEscapeAnalysis(const Program& program, Function& function)
        : m_program(program)
        , m_function(function)
        , m_length(program.find("length"_s.span8()).value_or(StringID()))
        , m_slice(program.find("slice"_s.span8()).value_or(StringID()))
        , m_getFromArguments(Builtins::singleton().find(BuiltinKind::Operation, { }, Builtins::singleton().nameOf("GetFromArguments"_s.span8())))
    {
    }

    bool run()
    {
        if (!findAllocations())
            return false;
        findUses();
        m_isStale.ensureSize(m_function.values.size());
        bool changed = false;
        for (unsigned i = 0; i < m_allocations.size(); ++i) {
            m_current = i + 1;
            changed |= tryReplace(m_allocations[i]);
        }
        return changed;
    }

private:
    struct Use {
        BlockID block;
        ValueID user;
    };

    enum class Kind : uint8_t { ArrayLiteral, ArgumentsObject, Rest, Closure };

    struct Allocation {
        Kind kind;
        // The allocation, then the casts through which it is looked at, each after its operand.
        Vector<Use, 2> aliases;
        Vector<Use, 2> uses;
        // Into a block parameter, or out of the function: it is not followed.
        bool flowsOn { false };
    };

    const Type& typeOf(ValueID value) const { return m_program.typeOf(m_function, value); }
    unsigned allocationOf(ValueID value) const { return value.index() < m_allocationOf.size() ? m_allocationOf[value.index()] : 0; }
    // Of the allocation that is being looked at.
    bool isAlias(ValueID value) const { return allocationOf(value) == m_current; }

    bool findAllocations()
    {
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            for (ValueID id : m_function.blocks[b].values) {
                auto& value = m_function.at(id);
                Kind kind;
                if (value.opcode == Opcode::NewArray && !mayThrow(m_program, m_function, value))
                    kind = Kind::ArrayLiteral;
                else if (value.opcode == Opcode::CreateRest)
                    kind = Kind::Rest;
                else if (value.opcode == Opcode::NewClosure)
                    kind = Kind::Closure;
                else if (languageOperationOf(m_program, value) == LanguageOperation::CreateArguments && value.arguments.size() == 1)
                    kind = Kind::ArgumentsObject;
                else
                    continue;
                if (m_allocations.isEmpty())
                    m_allocationOf.fill(0, m_function.values.size());
                m_allocations.append(Allocation { kind, { Use { BlockID(b), id } }, { }, false });
                m_allocationOf[id.index()] = m_allocations.size();
            }
        }
        if (m_allocations.isEmpty())
            return false;
        Type array = Type::engineClass(Builtins::singleton().arrayClass());
        Type object = Type::object();
        for (bool grew = true; grew;) {
            grew = false;
            for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
                for (ValueID id : m_function.blocks[b].values) {
                    auto& value = m_function.at(id);
                    if ((value.opcode != Opcode::Cast && value.opcode != Opcode::IteratorOpen) || allocationOf(id))
                        continue;
                    unsigned of = allocationOf(value.child(0));
                    if (!of)
                        continue;
                    if (m_allocations[of - 1].kind == Kind::Closure && (value.opcode != Opcode::Cast || value.castReason() != CastReason::Upcast))
                        continue;
                    // A new array stands for its own iterator: Array.prototype is immutable, and it has nothing in front of that.
                    if (value.opcode == Opcode::IteratorOpen ? m_allocations[of - 1].kind != Kind::ArrayLiteral : !isAliasingCast(value, m_allocations[of - 1].kind == Kind::ArgumentsObject ? object : array))
                        continue;
                    m_allocationOf[id.index()] = of;
                    m_allocations[of - 1].aliases.append(Use { BlockID(b), id });
                    grew = true;
                }
            }
        }
        return true;
    }

    void findUses()
    {
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            auto& block = m_function.blocks[b];
            for (ValueID id : block.values) {
                std::as_const(m_function.at(id)).forEachChild([&](ValueID child) {
                    unsigned of = allocationOf(child);
                    if (!of || of == allocationOf(id))
                        return;
                    auto& uses = m_allocations[of - 1].uses;
                    if (uses.isEmpty() || uses.last().user != id)
                        uses.append(Use { BlockID(b), id });
                });
            }
            block.terminator.forEachChild([&](ValueID& value) {
                if (unsigned of = allocationOf(value))
                    m_allocations[of - 1].flowsOn = true;
            });
        }
    }

    ValueID originOf(ValueID value) const
    {
        while (m_function.at(value).opcode == Opcode::Cast)
            value = m_function.at(value).child(0);
        return value;
    }

    // A cast that only tests, and that whatever the allocation makes passes.
    bool isAliasingCast(const Value& value, const Type& made) const
    {
        switch (value.castReason()) {
        case CastReason::Upcast:
            return true;
        case CastReason::Store:
        case CastReason::Claim:
        case CastReason::Flow:
        case CastReason::NonNull:
            return !Options::auditAOTTypedAssertions() && m_program.isHeldAlready(made, m_program.at(value.type), value.castReason());
        default:
            return false;
        }
    }

    std::optional<uint64_t> constantIndex(ValueID key) const
    {
        auto& value = m_function.at(originOf(key));
        if (value.opcode != Opcode::Integer || value.integer() < 0)
            return std::nullopt;
        return value.integer();
    }

    bool isApplyOfList(const Value& user) const
    {
        auto operation = languageOperationOf(m_program, user);
        return (operation == LanguageOperation::Apply || operation == LanguageOperation::CallWithSpread) && user.arguments.size() == 3
            && isAlias(user.arguments[2]) && !isAlias(user.arguments[0]) && !isAlias(user.arguments[1]);
    }

    // `[a, ...x]`, `f(a, ...x)`: AppendSpread target, x. Yields the target if that is an array literal nothing else has seen.
    const Allocation* targetOfSpread(const Value& user) const
    {
        if (languageOperationOf(m_program, user) != LanguageOperation::AppendSpread || user.arguments.size() != 2 || !isAlias(user.arguments[1]))
            return nullptr;
        unsigned of = allocationOf(user.arguments[0]);
        if (!of || of == m_current || !Type::engineClass(Builtins::singleton().arrayClass()).isWithin(m_program.at(user.type), m_program))
            return nullptr;
        auto& target = m_allocations[of - 1];
        if (target.kind != Kind::ArrayLiteral || target.flowsOn || target.uses.size() != 1 || m_isStale.get(target.aliases[0].user.index()))
            return nullptr;
        return &target;
    }

    // `Array.prototype.slice`, `[].slice`: Array.prototype is immutable, and a new array has nothing in front of it.
    bool isArrayPrototypeSlice(ValueID value) const
    {
        auto& builtins = Builtins::singleton();
        auto& read = m_function.at(originOf(value));
        if (read.opcode != Opcode::GetById || read.string() != m_slice)
            return false;
        auto& holder = m_function.at(originOf(read.child(0)));
        if (holder.opcode == Opcode::NewArray)
            return true;
        if (holder.opcode != Opcode::GetById || m_program.builtinName(holder.string()) != builtins.prototypeName())
            return false;
        auto& global = m_function.at(originOf(holder.child(0)));
        return global.opcode == Opcode::GetGlobal && m_program.builtinName(global.string()) == builtins.arrayClass().name && !m_program.isExtended(builtins.arrayClass().name);
    }

    // `Array.prototype.slice.call(arguments, ..)`, in any of the three ways it is written here. It copies the elements to a new array, as it
    // does for the array the arguments object was made from. Yields where the receiver is.
    ValueID* receiverOfSlice(Value& user) const
    {
        ValueID* receiver = nullptr;
        ValueID slice = user.child(0);
        bool isCallOfCall = false;
        if (user.opcode == Opcode::CallMethod)
            isCallOfCall = m_program.builtinName(user.string()) == Builtins::singleton().callName();
        else if (user.opcode != Opcode::Call)
            return nullptr;
        else if (auto& read = m_function.at(user.child(0)); read.opcode == Opcode::GetById && m_program.builtinName(read.string()) == Builtins::singleton().callName() && originOf(read.child(0)) == originOf(user.child(1))) {
            isCallOfCall = true;
            slice = user.child(1);
        } else
            receiver = &user.children[1];
        if (isCallOfCall && !user.arguments.isEmpty())
            receiver = &user.arguments[0];
        if (!receiver || !isAlias(*receiver) || !isArrayPrototypeSlice(slice))
            return nullptr;
        unsigned count = 0;
        std::as_const(user).forEachChild([&](ValueID child) {
            count += isAlias(child);
        });
        return count == 1 ? receiver : nullptr;
    }

    bool isGetFromArguments(const Value& user) const
    {
        return languageOperationOf(m_program, user) == LanguageOperation::GetFromArguments && user.arguments.size() == 2 && isAlias(user.arguments[0]) && !isAlias(user.arguments[1]);
    }

    // Of the array of a rest parameter. What is read is an argument, which may be anything: it is held to the type of the read as an element is.
    bool isReadOfRest(const Value& user) const
    {
        auto& type = m_program.at(user.type);
        if (user.opcode == Opcode::GetById && user.string() == m_length)
            return type.primitives & Primitive::Int && type.integers.contains(0) && type.integers.contains(std::numeric_limits<int32_t>::max());
        bool isAnything = m_program.at(m_program.types.anything).isWithin(type, m_program);
        if (user.opcode == Opcode::GetByVal && !isAlias(user.child(1)))
            return isAnything || (!Options::auditAOTTypedAssertions() && writesOfElementRead(m_program, type) == Writes::Nothing);
        return isAnything && isGetFromArguments(user);
    }

    // It goes through the array and is done with it: nobody else gets to see it.
    bool onlyBorrows(const Value& user) const
    {
        if (isApplyOfList(user))
            return true;
        auto operation = languageOperationOf(m_program, user);
        if (operation == LanguageOperation::CreateArguments)
            return user.arguments.size() == 1;
        return operation == LanguageOperation::AppendSpread && user.arguments.size() == 2 && isAlias(user.arguments[1]) && !isAlias(user.arguments[0]);
    }

    void replaceReadOfRest(const Use& use, uint32_t numberOfArgumentsToSkip)
    {
        auto& user = m_function.at(use.user);
        bool readsArgumentsObject = isGetFromArguments(user);
        bool isLength = user.opcode == Opcode::GetById;
        ValueID key = readsArgumentsObject ? user.arguments[1] : user.child(1);
        user.children = { };
        user.arguments.clear();
        user.immediates = { numberOfArgumentsToSkip, Format::none };
        if (isLength) {
            user.opcode = Opcode::GetRestLength;
            user.prediction = { };
            user.predictedClosure = { };
            if (m_program.at(m_program.types.int32).isWithin(m_program.at(user.type), m_program)) {
                user.type = m_program.types.int32;
                user.rep = repOf(m_program.at(user.type));
            }
        } else if (auto index = constantIndex(key); index && *index < maxArguments) {
            user.opcode = Opcode::GetArgument;
            user.immediates[0] += static_cast<uint32_t>(*index);
        } else {
            user.opcode = Opcode::GetMyArgumentByVal;
            user.children[0] = key;
            user.immediates[1] = readsArgumentsObject;
        }
        if (!mayThrow(m_program, m_function, user))
            stopWatching(use.block, use.user);
    }

    // Whether the use only reads, and there is something to put in its place. `elements`: of an array literal.
    bool isReplaceableRead(Value& user, const Arguments* elements) const
    {
        if (isApplyOfList(user))
            return true;
        if (!elements && receiverOfSlice(user))
            return true;
        if (elements && targetOfSpread(user))
            return true;
        auto& type = m_program.at(user.type);
        if (user.opcode == Opcode::GetById && user.string() == m_length)
            return !elements || (type.primitives & Primitive::Int && type.integers.contains(elements->size()));
        if (elements && user.opcode == Opcode::IteratorClose)
            return true;
        if (elements && user.opcode == Opcode::IteratorHasNext)
            return !!constantIndex(user.child(1));
        if ((user.opcode != Opcode::GetByVal && (!elements || user.opcode != Opcode::IteratorValue)) || isAlias(user.child(1)))
            return false;
        if (!elements)
            return true;
        auto index = constantIndex(user.child(1));
        if (!index)
            return false;
        if (*index >= elements->size())
            return type.primitives & Primitive::Undefined;
        return typeOf(elements->at(*index)).isWithin(type, m_program);
    }

    // The blocks that run after the block, on a path on which the allocation is not made anew.
    BitVector blocksAfter(BlockID block, BlockID allocatedIn) const
    {
        BitVector seen(m_function.blocks.size());
        Vector<BlockID> worklist { block };
        while (!worklist.isEmpty()) {
            for (auto& target : m_function.at(worklist.takeLast()).terminator.targets) {
                if (target.block != allocatedIn && !seen.set(target.block.index()))
                    worklist.append(target.block);
            }
        }
        return seen;
    }

    bool isLastUse(const Use& escape, BlockID allocatedIn) const
    {
        BitVector after = blocksAfter(escape.block, allocatedIn);
        if (after.get(escape.block.index()))
            return false;
        auto& values = m_function.at(escape.block).values;
        size_t place = values.find(escape.user);
        auto comesLater = [&](const Use& other) {
            if (other.user == escape.user)
                return false;
            return after.get(other.block.index()) || (other.block == escape.block && values.find(other.user) > place);
        };
        return !m_reads.containsIf(comesLater) && !m_borrows.containsIf(comesLater) && !m_escapes.containsIf(comesLater);
    }

    // The value no longer throws.
    void stopWatching(BlockID block, ValueID value)
    {
        auto& terminator = m_function.at(block).terminator;
        if (terminator.kind != TerminatorKind::Invoke || m_function.at(block).values.isEmpty() || m_function.at(block).values.last() != value)
            return;
        terminator.kind = TerminatorKind::Jump;
        terminator.targets.shrink(1);
    }

    void replaceRead(const Use& use, const Arguments* elements, ValueID list)
    {
        auto& user = m_function.at(use.user);
        if (isApplyOfList(user)) {
            if (!elements) {
                user.arguments[2] = list;
                return;
            }
            user.opcode = Opcode::Call;
            user.children = { user.arguments[0], user.arguments[1], ValueID() };
            user.immediates = { Format::none, Format::none };
            user.arguments = *elements;
            return;
        }
        if (ValueID* receiver = elements ? nullptr : receiverOfSlice(user)) {
            *receiver = list;
            return;
        }
        if (auto* target = elements ? targetOfSpread(user) : nullptr) {
            Arguments all = m_function.at(target->aliases[0].user).arguments;
            all.appendVector(*elements);
            user.opcode = Opcode::NewArray;
            user.immediates = { Format::none, Format::none };
            user.arguments = WTF::move(all);
            stopWatching(use.block, use.user);
            for (auto& alias : target->aliases)
                m_isStale.set(alias.user.index());
            return;
        }
        // What `arguments` has under a number, the array it was made from has too. Under a name it has other things.
        if (!elements && user.opcode == Opcode::GetByVal && !typeOf(user.child(1)).isNumber()) {
            user.opcode = Opcode::CallBuiltin;
            user.arguments = Arguments { list, user.child(1) };
            user.children = { };
            user.immediates = { m_getFromArguments.index(), Format::none };
            return;
        }
        if (!elements) {
            user.children[0] = list;
            return;
        }
        ValueID key = user.child(1);
        Opcode read = user.opcode;
        user.children = { };
        user.immediates = { Format::none, Format::none };
        if (read == Opcode::GetById) {
            user.opcode = Opcode::Integer;
            user.setBits(elements->size());
        } else if (read == Opcode::IteratorClose)
            user.opcode = Opcode::Undefined;
        else if (read == Opcode::IteratorHasNext) {
            user.opcode = Opcode::Boolean;
            user.immediates[0] = *constantIndex(key) < elements->size();
        } else if (uint64_t index = *constantIndex(key); index < elements->size()) {
            user.opcode = Opcode::Cast;
            user.children[0] = elements->at(index);
            user.immediates[1] = static_cast<uint32_t>(CastReason::Upcast);
        } else
            user.opcode = Opcode::Undefined;
        stopWatching(use.block, use.user);
    }

    // Puts a copy of the allocation, and of the casts between it and the use, in front of the use.
    void sinkTo(const Allocation& allocation, const Use& escape)
    {
        // Those on the way from the allocation to the use.
        Vector<ValueID, 4> needed;
        std::as_const(m_function.at(escape.user)).forEachChild([&](ValueID child) {
            needed.append(child);
        });
        for (unsigned i = allocation.aliases.size(); i-- > 1;) {
            if (needed.contains(allocation.aliases[i].user))
                needed.append(m_function.at(allocation.aliases[i].user).child(0));
        }
        Vector<std::pair<ValueID, ValueID>, 4> copies;
        for (auto& alias : allocation.aliases) {
            if (alias.user != allocation.aliases[0].user && !needed.contains(alias.user))
                continue;
            Value copy = m_function.at(alias.user);
            copy.forEachChild([&](ValueID& child) {
                for (auto& [original, itsCopy] : copies) {
                    if (child == original)
                        child = itsCopy;
                }
            });
            m_function.values.append(WTF::move(copy));
            copies.append({ alias.user, ValueID(m_function.values.size() - 1) });
        }
        auto& values = m_function.at(escape.block).values;
        size_t place = values.find(escape.user);
        for (auto& [original, copy] : copies)
            values.insert(place++, copy);
        m_function.at(escape.user).forEachChild([&](ValueID& child) {
            for (auto& [original, copy] : copies) {
                if (child == original)
                    child = copy;
            }
        });
    }

    ValueID withoutUpcasts(ValueID value) const
    {
        while (m_function.at(value).opcode == Opcode::Cast && m_function.at(value).castReason() == CastReason::Upcast)
            value = m_function.at(value).child(0);
        return value;
    }

    bool isLengthOfAllocation(ValueID value) const
    {
        auto& read = m_function.at(withoutUpcasts(value));
        return read.opcode == Opcode::GetById && read.string() == m_length && isAlias(read.child(0));
    }

    // What the condition comes to for an array of that length. Nothing: it does not go by the length of the allocation and a constant.
    std::optional<bool> decideByLength(ValueID condition, int64_t length) const
    {
        auto& value = m_function.at(withoutUpcasts(condition));
        switch (value.opcode) {
        case Opcode::LogicalNot:
            if (auto operand = decideByLength(value.child(0), length))
                return !*operand;
            return std::nullopt;
        case Opcode::ToBoolean:
            if (isLengthOfAllocation(value.child(0)))
                return !!length;
            return decideByLength(value.child(0), length);
        case Opcode::StrictEq:
        case Opcode::Eq:
        case Opcode::Less:
        case Opcode::LessEq:
        case Opcode::Greater:
        case Opcode::GreaterEq:
            break;
        default:
            return std::nullopt;
        }
        bool isLeft = isLengthOfAllocation(value.child(0));
        if (!isLeft && !isLengthOfAllocation(value.child(1)))
            return std::nullopt;
        auto& other = m_function.at(withoutUpcasts(value.child(isLeft)));
        if (other.opcode != Opcode::Integer)
            return std::nullopt;
        int64_t left = isLeft ? length : other.integer();
        int64_t right = isLeft ? other.integer() : length;
        switch (value.opcode) {
        case Opcode::Less:
            return left < right;
        case Opcode::LessEq:
            return left <= right;
        case Opcode::Greater:
            return left > right;
        case Opcode::GreaterEq:
            return left >= right;
        default:
            return left == right;
        }
    }

    struct DecidedBranch {
        BlockID block;
        bool isTaken;
    };

    // The blocks that are reached from the allocation of an array literal as long as the array is as it was made.
    BitVector blocksReachedWhileUnchanged(BlockID allocatedIn, int64_t length, Vector<DecidedBranch, 4>& decided) const
    {
        BitVector seen(m_function.blocks.size());
        seen.set(allocatedIn.index());
        Vector<BlockID> worklist { allocatedIn };
        while (!worklist.isEmpty()) {
            BlockID block = worklist.takeLast();
            auto& terminator = m_function.at(block).terminator;
            auto follow = [&](const Target& target) {
                if (!seen.set(target.block.index()))
                    worklist.append(target.block);
            };
            if (terminator.kind == TerminatorKind::Branch) {
                if (auto isTaken = decideByLength(terminator.value, length)) {
                    decided.append(DecidedBranch { block, *isTaken });
                    follow(terminator.targets[*isTaken ? 0 : 1]);
                    continue;
                }
            }
            for (auto& target : terminator.targets)
                follow(target);
        }
        return seen;
    }

    bool tryReplace(const Allocation& allocation)
    {
        if (allocation.flowsOn)
            return false;
        // What an earlier replacement has changed is looked at again the next time round.
        auto isStale = [&](const Use& use) {
            return m_isStale.get(use.user.index());
        };
        if (allocation.aliases.containsIf(isStale) || allocation.uses.containsIf(isStale))
            return false;

        BlockID block = allocation.aliases[0].block;
        Arguments operands = m_function.at(allocation.aliases[0].user).arguments;
        const Arguments* elements = allocation.kind == Kind::ArrayLiteral ? &operands : nullptr;
        ValueID list = allocation.kind == Kind::ArgumentsObject ? operands[0] : ValueID();
        uint32_t numberOfArgumentsToSkip = m_function.at(allocation.aliases[0].user).numberOfArgumentsToSkip();
        m_reads.clear();
        m_borrows.clear();
        m_escapes.clear();
        Vector<DecidedBranch, 4> decided;
        BitVector isReached;
        if (elements)
            isReached = blocksReachedWhileUnchanged(block, elements->size(), decided);
        for (auto& use : allocation.uses) {
            if (!decided.isEmpty() && !isReached.get(use.block.index()))
                continue;
            auto& user = m_function.at(use.user);
            if (user.opcode == Opcode::AssertNotAllocated)
                m_reads.append(use);
            else if (allocation.kind == Kind::Closure)
                m_escapes.append(use);
            else if (allocation.kind == Kind::Rest)
                (isReadOfRest(user) ? m_reads : onlyBorrows(user) ? m_borrows : m_escapes).append(use);
            else
                (isReplaceableRead(user, elements) ? m_reads : m_escapes).append(use);
        }
        // The arguments are passed on from where they are.
        auto isForwarded = [&](const Use& borrow) {
            auto& user = m_function.at(borrow.user);
            return isApplyOfList(user) && m_program.at(m_program.types.anything).isWithin(m_program.at(user.type), m_program);
        };
        // As it is, it is made once, where it is used.
        if (m_reads.isEmpty() && m_borrows.size() + m_escapes.size() == 1 && (m_borrows.isEmpty() ? m_escapes : m_borrows)[0].block == block && (m_borrows.isEmpty() || !isForwarded(m_borrows[0])))
            return false;
        for (auto& escape : m_escapes) {
            if (!isLastUse(escape, block))
                return false;
        }

        for (auto& read : m_reads) {
            if (m_function.at(read.user).opcode == Opcode::AssertNotAllocated)
                m_function.at(read.block).values.removeFirst(read.user);
            else if (allocation.kind == Kind::Rest)
                replaceReadOfRest(read, numberOfArgumentsToSkip);
            else
                replaceRead(read, elements, list);
        }
        for (auto& borrow : m_borrows) {
            if (isForwarded(borrow)) {
                auto& user = m_function.at(borrow.user);
                user.opcode = Opcode::CallForwardVarargs;
                user.children = { user.arguments[0], user.arguments[1], ValueID() };
                user.arguments.clear();
                user.immediates = { numberOfArgumentsToSkip, Format::none };
                continue;
            }
            sinkTo(allocation, borrow);
        }
        for (auto& escape : m_escapes)
            sinkTo(allocation, escape);
        if (!m_escapes.isEmpty() || !m_borrows.isEmpty())
            noteAllocation(m_program, m_function, m_function.at(allocation.aliases[0].user), AllocationOutcome::BuiltLate);
        for (auto& alias : allocation.aliases) {
            stopWatching(alias.block, alias.user);
            m_function.at(alias.block).values.removeFirst(alias.user);
            m_isStale.set(alias.user.index());
        }
        for (auto& use : allocation.uses)
            m_isStale.set(use.user.index());
        // What was not looked at is not reached.
        for (auto& branch : decided) {
            auto& terminator = m_function.at(branch.block).terminator;
            Target target = terminator.targets[branch.isTaken ? 0 : 1];
            terminator.kind = TerminatorKind::Jump;
            terminator.value = { };
            terminator.targets = { WTF::move(target) };
        }
        return true;
    }

    const Program& m_program;
    Function& m_function;
    StringID m_length;
    StringID m_slice;
    BuiltinID m_getFromArguments;
    Vector<Allocation> m_allocations;
    // By value: the number, from one, of the allocation that it is or is a cast of.
    Vector<unsigned> m_allocationOf;
    BitVector m_isStale;
    // The allocation that is being looked at, and its uses.
    unsigned m_current { 0 };
    Vector<Use, 4> m_reads;
    Vector<Use, 4> m_borrows;
    Vector<Use, 4> m_escapes;
};

std::optional<TypeofType> typeofTypeNamed(std::span<const uint8_t> text)
{
    static constexpr std::pair<ASCIILiteral, TypeofType> names[] = {
        { "undefined"_s, TypeofType::Undefined },
        { "bigint"_s, TypeofType::BigInt },
        { "boolean"_s, TypeofType::Boolean },
        { "number"_s, TypeofType::Number },
        { "string"_s, TypeofType::String },
        { "symbol"_s, TypeofType::Symbol },
        { "object"_s, TypeofType::Object },
        { "function"_s, TypeofType::Function },
    };
    for (auto& [name, type] : names) {
        if (equalSpans(name.span8(), text))
            return type;
    }
    return std::nullopt;
}

// A read of a field or a variable is what an earlier read of the same place yielded, if that one was carried out on every way here and nothing since may have stored there.
class RedundantReadElimination {
public:
    RedundantReadElimination(const Program& program, Function& function)
        : m_program(program)
        , m_function(function)
    {
    }

    bool run()
    {
        if (!findReads())
            return false;
        findAllocations();
        findActions();
        solve();
        return replace();
    }

private:
    static constexpr uint32_t none = UINT32_MAX;
    static uint64_t property(StringID name) { return name.index(); }
    static uint64_t environmentVariable(LayoutID layout, uint32_t field) { return 1ull << 62 | static_cast<uint64_t>(layout.index()) << 32 | field; }
    static uint64_t moduleVariable(VariableID variable) { return 2ull << 62 | variable.index(); }

    struct Read {
        uint64_t place;
        ValueID receiver;
        ValueID id;
        bool isFixed { false };
        // Of a store: what it stored. A read of the place yields that.
        ValueID stored { };

        ValueID yielded() const { return stored ? stored : id; }
    };

    struct Action {
        enum Kind : uint8_t { Reads, Clears, ClearsAll, Allocates, Escapes };
        Kind kind;
        uint32_t first { 0 };
        uint32_t second { 0 };
        // Of Clears: which of the objects the function has made the store goes to, if it is one.
        uint32_t allocation { none };
    };

    const Type& typeOf(ValueID id) const { return m_program.typeOf(m_function, id); }

    ValueID withoutTests(ValueID id) const
    {
        for (;;) {
            auto& value = m_function.at(id);
            if (value.opcode != Opcode::Cast || value.castReason() == CastReason::As || value.castReason() == CastReason::Coerce)
                return id;
            id = value.child(0);
        }
    }

    // What the function has made is of its layout, whatever else the type it is looked at as admits.
    bool isMadeWithField(const Value& access) const
    {
        auto& receiver = m_function.at(withoutTests(access.child(0)));
        return receiver.opcode == Opcode::NewObjectWithShape && isObjectLayout(m_program.at(receiver.layout()).kind) && m_program.at(receiver.layout()).fieldNamed(access.string());
    }

    std::optional<FieldAccess> fieldAccessedBy(const Value& access) const
    {
        if (isMadeWithField(access)) {
            LayoutID made = m_function.at(withoutTests(access.child(0))).layout();
            return FieldAccess { made, *m_program.at(made).fieldNamed(access.string()) };
        }
        return fieldOf(m_program, typeOf(access.child(0)), access.string());
    }

    bool isAccessToField(const Value& access)
    {
        auto found = fieldAccessedBy(access);
        if (!found)
            return false;
        // It has the field: only what it may inherit where the field is absent has a say.
        bool isMade = isMadeWithField(access);
        return m_onlyFieldsSupply.ensure(static_cast<uint64_t>(isMade) << 63 | static_cast<uint64_t>(found->layout.index()) << 32 | found->field, [&] {
            return isMade ? !mayInherit(m_program, found->layout, access.string()) : onlyFieldsSupply(m_program, found->layout, access.string());
        }).iterator->value;
    }

    std::optional<uint64_t> placeOfField(const Value& access) const
    {
        auto& layout = m_program.at(access.layout());
        if (!isObjectLayout(layout.kind))
            return environmentVariable(access.layout(), access.field());
        StringID name = layout.fields[access.field()].name;
        return name ? std::optional<uint64_t>(property(name)) : std::nullopt;
    }

    // As Lowering::lowerGetModuleVariable() and lowerGetEnvironmentField() have it: no call changes it.
    bool isFixed(const Value& read) const
    {
        if (read.opcode == Opcode::GetModuleVariable)
            return m_program.variables[read.variable().index()].isStoredOnce && !m_placesStoredTo.contains(moduleVariable(read.variable()));
        auto& layout = m_program.at(read.layout());
        if (isObjectLayout(layout.kind))
            return false;
        uint32_t flags = layout.fields[read.field()].flags;
        if (flags & fieldIsImmutable)
            return true;
        return flags & fieldIsStoredOnce && flags & fieldIsNeverSeenUninitialized && !m_placesStoredTo.contains(environmentVariable(read.layout(), read.field()));
    }

    std::optional<Read> readBy(ValueID id)
    {
        auto& value = m_function.at(id);
        switch (value.opcode) {
        case Opcode::GetModuleVariable:
            return Read { moduleVariable(value.variable()), ValueID(), id, isFixed(value) };
        case Opcode::GetField:
            if (auto place = placeOfField(value))
                return Read { *place, withoutTests(value.child(0)), id, isFixed(value) };
            return std::nullopt;
        case Opcode::GetById:
            // One that reads a method for a CallBuiltin may read nothing.
            if (value.immediates[1] != Format::none || !isAccessToField(value))
                return std::nullopt;
            return Read { property(value.string()), withoutTests(value.child(0)), id };
        case Opcode::PutModuleVariable:
            return Read { moduleVariable(value.variable()), ValueID(), id, false, value.child(0) };
        case Opcode::PutField:
            if (auto place = placeOfField(value))
                return Read { *place, withoutTests(value.child(0)), id, false, value.child(1) };
            return std::nullopt;
        case Opcode::PutById: {
            if (!isAccessToField(value))
                return std::nullopt;
            auto found = fieldAccessedBy(value);
            TypeID type = m_program.at(found->layout).fields[found->field].type;
            if (type && !typeOf(value.child(1)).isWithin(m_program.at(type), m_program))
                return std::nullopt;
            return Read { property(value.string()), withoutTests(value.child(0)), id, false, value.child(1) };
        }
        default:
            return std::nullopt;
        }
    }

    static bool isWatched(const Block& block, size_t index) { return block.terminator.kind == TerminatorKind::Invoke && index + 1 == block.values.size(); }

    // Numbers the reads that have another of the same place and receiver, by place, then by receiver: so those of one place are in a row, and in that those of one receiver.
    bool findReads()
    {
        for (auto& block : m_function.blocks) {
            for (ValueID id : block.values) {
                auto& value = m_function.at(id);
                if (value.opcode == Opcode::PutModuleVariable)
                    m_placesStoredTo.add(moduleVariable(value.variable()));
                else if (value.opcode == Opcode::PutField && !isObjectLayout(m_program.at(value.layout()).kind))
                    m_placesStoredTo.add(environmentVariable(value.layout(), value.field()));
            }
        }
        Vector<Read> reads;
        for (auto& block : m_function.blocks) {
            for (ValueID id : block.values) {
                if (auto read = readBy(id))
                    reads.append(*read);
            }
        }
        if (reads.size() < 2)
            return false;
        std::ranges::sort(reads, [](auto& a, auto& b) {
            if (a.isFixed != b.isFixed)
                return a.isFixed;
            if (a.place != b.place)
                return a.place < b.place;
            if (a.receiver != b.receiver)
                return a.receiver.index() < b.receiver.index();
            return a.id.index() < b.id.index();
        });
        m_bitOf.fill(none, m_function.values.size());
        for (size_t begin = 0; begin < reads.size();) {
            size_t end = begin + 1;
            bool hasRead = !reads[begin].stored;
            while (end < reads.size() && reads[end].isFixed == reads[begin].isFixed && reads[end].place == reads[begin].place && reads[end].receiver == reads[begin].receiver)
                hasRead |= !reads[end++].stored;
            if (end - begin > 1 && hasRead) {
                uint32_t first = m_reads.size();
                for (size_t i = begin; i < end; ++i) {
                    m_bitOf[reads[i].id.index()] = m_reads.size();
                    m_reads.append(reads[i]);
                    m_numberOfFixedBits += reads[i].isFixed;
                }
                for (size_t i = begin; i < end; ++i)
                    m_sameAs.append({ first, static_cast<uint32_t>(m_reads.size()) });
                auto& range = m_bitsOfPlace.add(reads[begin].place, std::pair<uint32_t, uint32_t> { first, 0 }).iterator->value;
                range.second = m_reads.size();
            }
            begin = end;
        }
        return !m_reads.isEmpty();
    }

    // The objects the function makes, of those that are receivers here. As long as it has handed one on to nobody, nothing but its own stores changes it. The bit that says so comes behind those of the reads.
    void findAllocations()
    {
        m_indexOfAllocation.fill(none, m_function.values.size());
        m_allocationOfRead.fill(none, m_reads.size());
        for (uint32_t bit = 0; bit < m_reads.size(); ++bit) {
            ValueID receiver = m_reads[bit].receiver;
            if (!receiver || m_function.at(receiver).opcode != Opcode::NewObjectWithShape || !isObjectLayout(m_program.at(m_function.at(receiver).layout()).kind))
                continue;
            uint32_t& index = m_indexOfAllocation[receiver.index()];
            if (index == none) {
                index = m_readsOfAllocation.size();
                m_readsOfAllocation.append(BitVector(m_reads.size()));
            }
            m_readsOfAllocation[index].quickSet(bit);
            m_allocationOfRead[bit] = index;
        }
    }

    unsigned numberOfBits() const { return m_reads.size() + m_readsOfAllocation.size(); }

    template<typename Functor>
    void forEachAllocationHandedOnBy(const Value& value, const Functor& functor)
    {
        auto handOn = [&](ValueID child) {
            if (!child)
                return;
            if (uint32_t allocation = m_indexOfAllocation[withoutTests(child).index()]; allocation != none)
                functor(allocation);
        };
        switch (value.opcode) {
        case Opcode::AssertNotAllocated:
            return;
        // It stands for the object: see withoutTests().
        case Opcode::Cast:
            if (value.castReason() != CastReason::As && value.castReason() != CastReason::Coerce)
                return;
            break;
        case Opcode::StrictEq:
        case Opcode::IsUndefinedOrNull:
        case Opcode::HasShapeOf:
        case Opcode::TypeOf:
        case Opcode::TypeOfIs:
            return;
        case Opcode::GetField:
            return;
        case Opcode::PutField:
            handOn(value.child(1));
            return;
        case Opcode::GetById:
        case Opcode::InById:
            if (isAccessToField(value))
                return;
            break;
        case Opcode::PutById:
            if (isAccessToField(value)) {
                handOn(value.child(1));
                return;
            }
            break;
        default:
            break;
        }
        value.forEachChild(handOn);
    }

    Action clears(std::optional<uint64_t> place) const
    {
        if (!place)
            return { Action::ClearsAll };
        auto found = m_bitsOfPlace.find(*place);
        if (found == m_bitsOfPlace.end())
            return { Action::Clears };
        return { Action::Clears, found->value.first, found->value.second };
    }

    void findActions()
    {
        m_actions.grow(m_function.blocks.size());
        m_bitSetUnlessItThrows.fill(none, m_function.blocks.size());
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            auto& block = m_function.blocks[b];
            auto add = [&](Action action) {
                if (action.kind != Action::Clears || action.first != action.second)
                    m_actions[b].append(action);
            };
            auto through = [&](const Value& store, Action action) {
                if (!m_readsOfAllocation.isEmpty())
                    action.allocation = m_indexOfAllocation[withoutTests(store.child(0)).index()];
                return action;
            };
            for (size_t i = 0; i < block.values.size(); ++i) {
                auto& value = m_function.at(block.values[i]);
                if (!m_readsOfAllocation.isEmpty()) {
                    if (uint32_t allocation = m_indexOfAllocation[block.values[i].index()]; allocation != none)
                        add({ Action::Allocates, allocation });
                    forEachAllocationHandedOnBy(std::as_const(value), [&](uint32_t allocation) {
                        add({ Action::Escapes, allocation });
                    });
                }
                switch (value.opcode) {
                case Opcode::GetModuleVariable:
                case Opcode::GetField:
                    break;
                case Opcode::GetById:
                case Opcode::InById:
                    if (!isAccessToField(value))
                        add({ Action::ClearsAll });
                    break;
                case Opcode::PutById:
                    add(through(value, clears(isAccessToField(value) ? std::optional<uint64_t>(property(value.string())) : std::nullopt)));
                    break;
                case Opcode::PutField:
                    add(through(value, clears(placeOfField(value))));
                    break;
                case Opcode::PutModuleVariable:
                    add(clears(moduleVariable(value.variable())));
                    break;
                case Opcode::CallDirect:
                case Opcode::Await:
                case Opcode::Yield:
                case Opcode::GetResumeMode:
                    add({ Action::ClearsAll });
                    continue;
                default:
                    if (writesOf(m_program, m_function, block, i) >= Writes::Fields)
                        add({ Action::ClearsAll });
                    continue;
                }
                if (uint32_t bit = m_bitOf[block.values[i].index()]; bit != none) {
                    if (isWatched(block, i))
                        m_bitSetUnlessItThrows[b] = bit;
                    else
                        add({ Action::Reads, bit, static_cast<uint32_t>(i) });
                }
            }
            if (!m_readsOfAllocation.isEmpty()) {
                block.terminator.forEachChild([&](ValueID& child) {
                    if (uint32_t allocation = m_indexOfAllocation[withoutTests(child).index()]; allocation != none)
                        add({ Action::Escapes, allocation });
                });
            }
        }
    }

    void carryOut(const Action& action, BitVector& bits) const
    {
        switch (action.kind) {
        case Action::Reads:
            bits.quickSet(action.first);
            return;
        case Action::Clears:
            for (uint32_t bit = action.first; bit < action.second; ++bit) {
                // Nobody else has what the function has made and handed on to nobody: a store through another receiver goes elsewhere.
                if (uint32_t allocation = m_readsOfAllocation.isEmpty() ? none : m_allocationOfRead[bit]; allocation != none && allocation != action.allocation && bits.quickGet(m_reads.size() + allocation))
                    continue;
                bits.quickClear(bit);
            }
            return;
        case Action::ClearsAll:
            if (!m_readsOfAllocation.isEmpty()) {
                // Few are still with the function at any one place.
                BitVector kept = m_fixed;
                for (size_t bit = bits.findBit(m_reads.size(), true); bit < numberOfBits(); bit = bits.findBit(bit + 1, true))
                    kept.merge(m_readsOfAllocation[bit - m_reads.size()]);
                bits.filter(kept);
            } else if (!m_numberOfFixedBits)
                bits.clearAll();
            else
                bits.filter(m_fixed);
            return;
        case Action::Allocates:
            bits.quickSet(m_reads.size() + action.first);
            return;
        case Action::Escapes:
            bits.quickClear(m_reads.size() + action.first);
            return;
        }
    }

    BitVector atStartOf(unsigned block) const
    {
        if (!block)
            return BitVector(numberOfBits());
        BitVector bits = m_all;
        for (unsigned predecessor : m_predecessors[block]) {
            // What a handler watches has been carried out where the block goes on, and not where the handler is.
            uint32_t bit = m_bitSetUnlessItThrows[predecessor];
            auto& terminator = m_function.blocks[predecessor].terminator;
            bool isCarriedOut = bit != none && bits.quickGet(bit) && (terminator.kind != TerminatorKind::Invoke || terminator.targets[1].block.index() != block);
            bits.filter(m_atEnd[predecessor]);
            if (isCarriedOut)
                bits.quickSet(bit);
        }
        return bits;
    }

    void solve()
    {
        unsigned numberOfBlocks = m_function.blocks.size();
        m_predecessors.grow(numberOfBlocks);
        auto& predecessors = m_predecessors;
        auto& order = m_order;
        {
            BitVector seen(numberOfBlocks);
            seen.set(0);
            Vector<std::pair<unsigned, unsigned>> stack { { 0u, 0u } };
            while (!stack.isEmpty()) {
                auto& [block, next] = stack.last();
                auto& targets = m_function.blocks[block].terminator.targets;
                if (next == targets.size()) {
                    order.append(block);
                    stack.removeLast();
                    continue;
                }
                unsigned target = targets[next++].block.index();
                predecessors[target].append(block);
                if (!seen.set(target))
                    stack.append({ target, 0u });
            }
            order.reverse();
        }
        m_all.ensureSize(numberOfBits());
        for (unsigned bit = 0; bit < numberOfBits(); ++bit)
            m_all.quickSet(bit);
        // No call hands an object on that it does not get.
        m_fixed.ensureSize(numberOfBits());
        for (unsigned bit = 0; bit < m_numberOfFixedBits; ++bit)
            m_fixed.quickSet(bit);
        for (unsigned bit = m_reads.size(); bit < numberOfBits(); ++bit)
            m_fixed.quickSet(bit);
        // Until it is known better, everything holds behind a block that has not been looked at: so a loop does not clear what its body leaves alone.
        m_atEnd.fill(m_all, numberOfBlocks);
        for (bool changed = true; changed;) {
            changed = false;
            for (unsigned block : order) {
                BitVector bits = atStartOf(block);
                for (auto& action : m_actions[block])
                    carryOut(action, bits);
                if (bits != m_atEnd[block]) {
                    m_atEnd[block] = WTF::move(bits);
                    changed = true;
                }
            }
        }
    }

    bool replace()
    {
        bool changed = false;
        for (unsigned b : m_order) {
            if (m_actions[b].isEmpty() && m_bitSetUnlessItThrows[b] == none)
                continue;
            auto& block = m_function.blocks[b];
            BitVector bits = atStartOf(b);
            auto tryReplace = [&](uint32_t bit, ValueID id) {
                if (m_reads[bit].stored)
                    return false;
                auto& value = m_function.at(id);
                auto [first, end] = m_sameAs[bit];
                for (uint32_t other = first; other < end; ++other) {
                    if (!bits.quickGet(other) || other == bit)
                        continue;
                    ValueID earlier = m_reads[other].yielded();
                    if (!typeOf(earlier).isWithin(m_program.at(value.type), m_program))
                        continue;
                    value.opcode = Opcode::Cast;
                    value.children = { earlier, ValueID(), ValueID() };
                    value.immediates = { Format::none, static_cast<uint32_t>(CastReason::Upcast) };
                    return true;
                }
                return false;
            };
            for (auto& action : m_actions[b]) {
                if (action.kind == Action::Reads)
                    changed |= tryReplace(action.first, block.values[action.second]);
                carryOut(action, bits);
            }
            // It no longer throws.
            if (uint32_t bit = m_bitSetUnlessItThrows[b]; bit != none && tryReplace(bit, block.values.last())) {
                block.terminator.kind = TerminatorKind::Jump;
                block.terminator.targets.shrink(1);
                changed = true;
            }
        }
        return changed;
    }

    const Program& m_program;
    Function& m_function;
    Vector<Read> m_reads;
    Vector<std::pair<uint32_t, uint32_t>> m_sameAs;
    Vector<uint32_t> m_bitOf;
    // By value: see findAllocations().
    Vector<uint32_t> m_indexOfAllocation;
    Vector<BitVector> m_readsOfAllocation;
    Vector<uint32_t> m_allocationOfRead;
    UncheckedKeyHashMap<uint64_t, std::pair<uint32_t, uint32_t>, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_bitsOfPlace;
    UncheckedKeyHashMap<uint64_t, bool, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_onlyFieldsSupply;
    Vector<Vector<Action>> m_actions;
    // By block: the read or store that a handler watches there.
    Vector<uint32_t> m_bitSetUnlessItThrows;
    Vector<Vector<unsigned, 2>> m_predecessors;
    Vector<BitVector> m_atEnd;
    Vector<unsigned> m_order;
    BitVector m_all;
    BitVector m_fixed;
    unsigned m_numberOfFixedBits { 0 };
    UncheckedKeyHashSet<uint64_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_placesStoredTo;
};

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

    void moveBirthsIntoArms()
    {
        if (m_function.inlineFrames.isEmpty())
            return;
        m_movesBirths = true;
        moveStoresIntoArms();
        gatherInitializingStores();
    }

    void run()
    {
        if (m_function.blocks.isEmpty())
            return;
        nameConstantKeys();
        foldReads();
        foldTypeOfComparisons();
        foldSpreadCalls();
        foldSuperConstructs();
        do {
            bool changed;
            do {
                foldInvokes();
                foldConstantConditions();
                foldBranches();
                dropUnreachedBlocks();
                changed = removeTrivialParameters();
                changed |= mergeChains();
                changed |= forwardStores();
                changed |= removeUnusedValues();
            } while (changed);
        } while (!Options::auditAOTTypedAssertions() && (RedundantReadElimination(m_program, m_function).run() || ArrayEscapeAnalysis(m_program, m_function).run() || promoteEnvironments()));
        moveStoresIntoArms();
        gatherInitializingStores();
    }

private:
    // `o["name"]` is `o.name`. From here on there is one form of it.
    void nameConstantKeys()
    {
        for (auto& value : m_function.values) {
            Opcode byName;
            switch (value.opcode) {
            case Opcode::GetByVal:
                byName = Opcode::GetById;
                break;
            case Opcode::PutByVal:
                byName = Opcode::PutById;
                break;
            case Opcode::InByVal:
                byName = Opcode::InById;
                break;
            case Opcode::DeleteByVal:
                byName = Opcode::DeleteById;
                break;
            default:
                continue;
            }
            ValueID key = value.child(1);
            while (m_function.at(key).opcode == Opcode::Cast && m_function.at(key).castReason() == CastReason::Upcast)
                key = m_function.at(key).child(0);
            if (m_function.at(key).opcode != Opcode::String)
                continue;
            StringID name = m_function.at(key).string();
            auto text = m_program.string(name);
            // An index is an element, and a typed array has its own answer to whatever reads like a number.
            if (text.empty() || isASCIIDigit(text[0]) || text[0] == '-' || equalSpans(text, "Infinity"_s.span8()) || equalSpans(text, "NaN"_s.span8()) || isPrivateName(text))
                continue;
            ValueID stored = value.opcode == Opcode::PutByVal ? value.child(2) : ValueID();
            if (stored) {
                // Only what is of the type of the field is stored to it by name.
                auto access = fieldOf(m_program, m_program.typeOf(m_function, value.child(0)), name);
                TypeID held = access ? m_program.at(access->layout).fields[access->field].type : TypeID();
                if (held && !m_program.typeOf(m_function, stored).isWithin(m_program.at(held), m_program))
                    continue;
            }
            value.opcode = byName;
            value.children = { value.child(0), stored, ValueID() };
            value.immediates = { name.index(), Format::none };
        }
    }

    struct FieldOfLocalObject {
        ValueID object;
        StringID name;
    };

    // The field the value reads or stores to, if that is a field of an object this function has made and no code runs. No other object is it.
    std::optional<FieldOfLocalObject> fieldOfLocalObject(const Value& access) const
    {
        ValueID object = access.child(0);
        while (m_function.at(object).opcode == Opcode::Cast)
            object = m_function.at(object).child(0);
        // An environment may have two variables of one name.
        if (m_function.at(object).opcode != Opcode::NewObjectWithShape || !isObjectLayout(m_program.at(m_function.at(object).layout()).kind))
            return std::nullopt;
        bool isByName = access.opcode == Opcode::GetById || access.opcode == Opcode::PutById;
        StringID name = isByName ? access.string() : m_program.at(access.layout()).fields[access.field()].name;
        if (!name || !m_program.at(m_function.at(object).layout()).fieldNamed(name))
            return std::nullopt;
        // Where the field is absent the prototype chain is asked, and an accessor there runs.
        if (isByName && mayInherit(m_program, m_function.at(object).layout(), name))
            return std::nullopt;
        return FieldOfLocalObject { object, name };
    }

    // `o.f = v; .. o.f ..`: what is read is what was stored. Through a view in which some member lacks the field the read is typed as anything
    // and held to its type again, which the back end does not see through.
    bool forwardStores()
    {
        bool changed = false;
        // By the object and the name: what the field is known to hold at this point of the block.
        UncheckedKeyHashMap<uint64_t, ValueID, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> held;
        // Where there is one way into a block, and it is not an Invoke's way into its handler (how far what threw had come is not known there; a Throw ends its block, all of which has been carried out): there it holds on.
        Vector<unsigned> waysIn;
        waysIn.fill(0, m_function.blocks.size());
        waysIn[0] = 1;
        for (auto& block : m_function.blocks) {
            for (auto& target : block.terminator.targets)
                ++waysIn[target.block.index()];
        }
        BitVector isContinuation(m_function.blocks.size());
        for (auto& block : m_function.blocks) {
            for (size_t i = 0; i < numberOfWaysOn(block); ++i) {
                if (waysIn[block.terminator.targets[i].block.index()] == 1)
                    isContinuation.set(block.terminator.targets[i].block.index());
            }
        }
        for (unsigned head = 0; head < m_function.blocks.size(); ++head) {
            if (isContinuation.get(head))
                continue;
            held.clear();
            m_absent.clear();
            forwardStoresFrom(head, isContinuation, held, changed);
        }
        return changed;
    }

    static size_t numberOfWaysOn(const Block& block) { return block.terminator.kind == TerminatorKind::Invoke ? 1 : block.terminator.targets.size(); }

    void forwardStoresFrom(unsigned head, const BitVector& isContinuation, UncheckedKeyHashMap<uint64_t, ValueID, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>& held, bool& changed)
    {
        // The other ways on of the blocks that have been passed, each with what held at the end of its block.
        struct WayOn {
            unsigned block;
            UncheckedKeyHashMap<uint64_t, ValueID, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> held;
            UncheckedKeyHashSet<uint64_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> absent;
        };
        Vector<WayOn> later;
        for (unsigned b = head;;) {
            auto& block = m_function.blocks[b];
            Vector<unsigned, 2> waysOn;
            for (size_t i = 0; i < numberOfWaysOn(block); ++i) {
                if (isContinuation.get(block.terminator.targets[i].block.index()))
                    waysOn.append(block.terminator.targets[i].block.index());
            }
            for (size_t i = 0; i < block.values.size(); ++i) {
                auto& access = m_function.at(block.values[i]);
                bool isRead = access.opcode == Opcode::GetById || access.opcode == Opcode::GetField;
                bool isStore = access.opcode == Opcode::PutById || access.opcode == Opcode::PutField;
                auto accessed = isRead || isStore ? fieldOfLocalObject(access) : std::nullopt;
                ValueID literal = literalDefinedOn(access);
                forgetLiteralsUsedBy(access, literal);
                if (!accessed) {
                    if (literal)
                        held.set(static_cast<uint64_t>(literal.index()) << 32 | m_function.at(access.arguments[1]).string().index(), access.arguments[2]);
                    else if (access.opcode == Opcode::NewObject)
                        m_absent.add(static_cast<uint64_t>(block.values[i].index()) << 32 | UINT32_MAX);
                    else if (access.opcode == Opcode::NewObjectWithShape && !access.child(0)) {
                        // Under no name: it has nothing but its fields.
                        m_absent.add(static_cast<uint64_t>(block.values[i].index()) << 32 | UINT32_MAX);
                        for (auto& field : m_program.at(access.layout()).fields) {
                            if (field.name)
                                m_absent.add(static_cast<uint64_t>(block.values[i].index()) << 32 | field.name.index());
                        }
                    } else if (findsNothing(access) || findsNothingInLiteral(access, held)) {
                        bool isQuestion = access.opcode == Opcode::InById;
                        access.opcode = isQuestion ? Opcode::Boolean : Opcode::Undefined;
                        access.children = { };
                        access.immediates = { isQuestion ? 0 : Format::none, Format::none };
                        changed = true;
                        if (i + 1 == block.values.size() && block.terminator.kind == TerminatorKind::Invoke) {
                            block.terminator.kind = TerminatorKind::Jump;
                            block.terminator.targets.shrink(1);
                        }
                    } else if (isStore || (!doesNothing(access) && mayWriteFields(block, i))) {
                        // A store that is to no field of what the object was made as (through a wider view, say) adds a property, whatever it says of other objects' fields.
                        m_absent.clear();
                        if (!doesNothing(access) && mayWriteFields(block, i))
                            held.clear();
                    }
                    continue;
                }
                uint64_t key = static_cast<uint64_t>(accessed->object.index()) << 32 | accessed->name.index();
                if (isStore) {
                    m_absent.remove(key);
                    // What is not of the type of the field may be made something else on its way in.
                    auto& layout = m_program.at(m_function.at(accessed->object).layout());
                    TypeID typeOfField = layout.fields[*layout.fieldNamed(accessed->name)].type;
                    if (typeOfField && !m_program.typeOf(m_function, access.child(1)).isWithin(m_program.at(typeOfField), m_program))
                        held.remove(key);
                    else
                        held.set(key, access.child(1));
                    continue;
                }
                auto found = held.find(key);
                if (found == held.end()) {
                    if (!m_absent.contains(key) || !(m_program.at(access.type).primitives & Primitive::Undefined))
                        continue;
                    access.opcode = Opcode::Undefined;
                    access.children = { };
                    access.immediates = { Format::none, Format::none };
                    changed = true;
                    if (i + 1 == block.values.size() && block.terminator.kind == TerminatorKind::Invoke) {
                        block.terminator.kind = TerminatorKind::Jump;
                        block.terminator.targets.shrink(1);
                    }
                    continue;
                }
                if (!m_program.typeOf(m_function, found->value).isWithin(m_program.at(access.type), m_program)) {
                    held.remove(found);
                    continue;
                }
                ValueID value = found->value;
                access.opcode = Opcode::Cast;
                access.children = { value, ValueID(), ValueID() };
                access.immediates = { Format::none, static_cast<uint32_t>(CastReason::Upcast) };
                changed = true;
                // It no longer throws.
                if (i + 1 == block.values.size() && block.terminator.kind == TerminatorKind::Invoke) {
                    block.terminator.kind = TerminatorKind::Jump;
                    block.terminator.targets.shrink(1);
                }
            }
            block.terminator.forEachChild([&](ValueID& child) {
                forgetLiteral(child);
            });
            if (waysOn.isEmpty()) {
                if (later.isEmpty())
                    return;
                auto last = later.takeLast();
                b = last.block;
                held = WTF::move(last.held);
                m_absent = WTF::move(last.absent);
                continue;
            }
            for (size_t i = 0; i + 1 < waysOn.size(); ++i)
                later.append(WayOn { waysOn[i], held, m_absent });
            b = waysOn.last();
        }
    }

    // By the object and the name: the fields that nothing has stored since the object was made, at this point of the block.
    UncheckedKeyHashSet<uint64_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_absent;

    // A read by a name that is no field, or the question whether there is such a property, of a record that was just made and has nothing but its fields yet: neither has it one nor does it inherit one.
    bool findsNothing(const Value& read) const
    {
        if (read.opcode != Opcode::InById && (read.opcode != Opcode::GetById || !(m_program.at(read.type).primitives & Primitive::Undefined)))
            return false;
        ValueID object = read.child(0);
        while (m_function.at(object).opcode == Opcode::Cast)
            object = m_function.at(object).child(0);
        if (m_function.at(object).opcode != Opcode::NewObjectWithShape || !m_absent.contains(static_cast<uint64_t>(object.index()) << 32 | UINT32_MAX))
            return false;
        LayoutID layout = m_function.at(object).layout();
        StringID name = read.string();
        if (m_program.at(layout).kind != LayoutKind::Record || m_program.at(layout).fieldNamed(name) || isPrivateName(m_program.string(name)) || mayInherit(m_program, layout, name))
            return false;
        // The accessors that the type of an object literal declares are put on the object behind the allocation.
        for (LayoutID ancestor = layout; ancestor; ancestor = m_program.at(ancestor).base) {
            if (m_program.at(ancestor).hasMethodOfUnknownName)
                return false;
            for (auto& method : m_program.at(ancestor).methods) {
                if (method.name == name)
                    return false;
            }
        }
        return true;
    }

    static bool onlyTests(const Value& cast) { return cast.castReason() != CastReason::As && cast.castReason() != CastReason::Coerce; }

    void forgetLiteral(ValueID object)
    {
        while (m_function.at(object).opcode == Opcode::Cast)
            object = m_function.at(object).child(0);
        if (m_function.at(object).opcode == Opcode::NewObject)
            m_absent.remove(static_cast<uint64_t>(object.index()) << 32 | UINT32_MAX);
    }

    // The ordinary object that was just made, which nothing has yet, on which the value defines a property under a name that is written there. No code runs.
    ValueID literalDefinedOn(const Value& value) const
    {
        if (languageOperationOf(m_program, value) != LanguageOperation::DefineField || value.arguments.size() != 3 || m_function.at(value.arguments[1]).opcode != Opcode::String)
            return { };
        ValueID object = value.arguments[0];
        if (m_function.at(object).opcode != Opcode::NewObject || !m_absent.contains(static_cast<uint64_t>(object.index()) << 32 | UINT32_MAX))
            return { };
        return object;
    }

    // Such an object is known for as long as it is only asked by name and defined on. Any other use, and it may be anybody's, with any prototype and any property.
    void forgetLiteralsUsedBy(const Value& user, ValueID definedOn)
    {
        bool asks = user.opcode == Opcode::GetById || user.opcode == Opcode::InById || (user.opcode == Opcode::Cast && onlyTests(user));
        if (asks)
            return;
        for (ValueID child : user.children) {
            if (child)
                forgetLiteral(child);
        }
        for (size_t i = 0; i < user.arguments.size(); ++i) {
            if (!(definedOn && !i))
                forgetLiteral(user.arguments[i]);
        }
    }

    // findsNothing() of such an object: it has what has been defined on it, and inherits what Object.prototype has.
    bool findsNothingInLiteral(const Value& read, const UncheckedKeyHashMap<uint64_t, ValueID, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>& held) const
    {
        if (read.opcode != Opcode::InById && (read.opcode != Opcode::GetById || !(m_program.at(read.type).primitives & Primitive::Undefined)))
            return false;
        ValueID object = read.child(0);
        while (m_function.at(object).opcode == Opcode::Cast)
            object = m_function.at(object).child(0);
        if (m_function.at(object).opcode != Opcode::NewObject || !m_absent.contains(static_cast<uint64_t>(object.index()) << 32 | UINT32_MAX))
            return false;
        StringID name = read.string();
        return !held.contains(static_cast<uint64_t>(object.index()) << 32 | name.index()) && !isPrivateName(m_program.string(name)) && !isOnObjectPrototype(m_program, name);
    }

    bool mayWriteFields(const Block& block, size_t index) const
    {
        switch (m_function.at(block.values[index]).opcode) {
        // What these change is summed up for whole functions, later: writesOf() has no answer.
        case Opcode::CallDirect:
        case Opcode::Await:
        case Opcode::Yield:
        case Opcode::GetResumeMode:
            return true;
        default:
            return writesOf(m_program, m_function, block, index) >= Writes::Fields;
        }
    }

    // An environment that nothing gets to see but the reads and the stores of its variables in this function is not made. A variable is what was
    // last stored to it, and block parameters hand that on. It is what is left of `let s = 0; a.forEach(x => { s += x; })` once the callback
    // has been inlined and its closure is gone.
    bool promoteEnvironments()
    {
        // By value: the block an environment is made in, from one. 0: it is none, or something else gets to see it.
        Vector<unsigned> madeIn;
        madeIn.fill(0, m_function.values.size());
        bool any = false;
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            for (ValueID id : m_function.blocks[b].values) {
                auto& value = m_function.at(id);
                if (value.opcode != Opcode::NewObjectWithShape || value.child(0))
                    continue;
                auto& layout = m_program.at(value.layout());
                if (layout.kind != LayoutKind::Environment || layout.isCoroutineFrame)
                    continue;
                madeIn[id.index()] = b + 1;
                any = true;
            }
        }
        if (!any)
            return false;
        for (auto& block : m_function.blocks) {
            for (ValueID id : block.values) {
                auto& user = m_function.at(id);
                bool isAccess = user.opcode == Opcode::GetField || user.opcode == Opcode::PutField;
                std::as_const(user).forEachChild([&](ValueID child) {
                    if (!madeIn[child.index()] || (isAccess && user.child(0) == child && user.child(1) != child && user.layout() == m_function.at(child).layout()))
                        return;
                    madeIn[child.index()] = 0;
                });
            }
            block.terminator.forEachChild([&](ValueID& value) {
                madeIn[value.index()] = 0;
            });
        }
        for (unsigned v = 0; v < madeIn.size(); ++v) {
            if (madeIn[v] && promote(ValueID(v), madeIn[v] - 1))
                return true;
        }
        return false;
    }

    bool promote(ValueID environment, unsigned madeIn)
    {
        auto& fields = m_program.at(m_function.at(environment).layout()).fields;
        unsigned numberOfBlocks = m_function.blocks.size();
        static constexpr unsigned maxBlocks = 4096;
        if (fields.size() > 64 || numberOfBlocks > maxBlocks)
            return false;
        auto typeOfField = [&](unsigned field) {
            return fields[field].type ? fields[field].type : m_program.types.anything;
        };
        auto isAccess = [&](const Value& value) {
            return (value.opcode == Opcode::GetField || value.opcode == Opcode::PutField) && value.child(0) == environment;
        };

        // By block, a bit a variable: read before anything is stored to it there; stored to there.
        Vector<uint64_t> readFirst;
        Vector<uint64_t> stored;
        readFirst.fill(0, numberOfBlocks);
        stored.fill(0, numberOfBlocks);
        for (unsigned b = 0; b < numberOfBlocks; ++b) {
            for (ValueID id : m_function.blocks[b].values) {
                auto& access = m_function.at(id);
                if (!isAccess(access))
                    continue;
                auto& type = m_program.at(typeOfField(access.field()));
                uint64_t bit = 1ull << access.field();
                if (access.opcode == Opcode::PutField) {
                    if (!m_program.typeOf(m_function, access.child(1)).isWithin(type, m_program))
                        return false;
                    stored[b] |= bit;
                    continue;
                }
                if (!type.isWithin(m_program.at(access.type), m_program))
                    return false;
                if (!(stored[b] & bit))
                    readFirst[b] |= bit;
            }
        }

        // What may be read before it is stored to again, on the way in. Whoever gets to where the environment is made is done with this one.
        Vector<uint64_t> live = readFirst;
        for (bool changed = true; changed;) {
            changed = false;
            for (unsigned b = numberOfBlocks; b--;) {
                uint64_t after = 0;
                for (auto& target : m_function.blocks[b].terminator.targets) {
                    if (target.block.index() != madeIn)
                        after |= live[target.block.index()];
                }
                uint64_t before = readFirst[b] | (after & ~stored[b]);
                if (before != live[b]) {
                    live[b] = before;
                    changed = true;
                }
            }
        }
        // A variable that may be read before anything is stored to it: to read it then throws.
        if (live[madeIn])
            return false;

        Vector<Vector<ValueID>> handedOn(numberOfBlocks);
        for (unsigned b = 0; b < numberOfBlocks; ++b) {
            if (!live[b])
                continue;
            handedOn[b].fill(ValueID(), fields.size());
            auto& block = m_function.blocks[b];
            for (unsigned field = 0; field < fields.size(); ++field) {
                if (!(live[b] & 1ull << field))
                    continue;
                Value parameter;
                parameter.opcode = Opcode::Parameter;
                parameter.position = m_function.at(environment).position;
                parameter.inlineFrame = m_function.at(environment).inlineFrame;
                parameter.type = typeOfField(field);
                parameter.rep = repOf(m_program.at(parameter.type));
                parameter.immediates = { b, static_cast<uint32_t>(block.parameters.size()) };
                m_function.values.append(WTF::move(parameter));
                handedOn[b][field] = ValueID(m_function.values.size() - 1);
                block.parameters.append(handedOn[b][field]);
            }
        }
        Vector<ValueID> held;
        for (unsigned b = 0; b < numberOfBlocks; ++b) {
            auto& block = m_function.blocks[b];
            if (live[b])
                held = handedOn[b];
            else
                held.fill(ValueID(), fields.size());
            for (size_t i = 0; i < block.values.size(); ++i) {
                auto& access = m_function.at(block.values[i]);
                if (!isAccess(access))
                    continue;
                unsigned field = access.field();
                if (access.opcode == Opcode::PutField) {
                    held[field] = access.child(1);
                    access.opcode = Opcode::Undefined;
                    access.children = { };
                    access.immediates = { Format::none, Format::none };
                } else {
                    RELEASE_ASSERT(held[field]);
                    access.opcode = Opcode::Cast;
                    access.children = { held[field], ValueID(), ValueID() };
                    access.immediates = { Format::none, static_cast<uint32_t>(CastReason::Upcast) };
                }
                // It no longer throws.
                if (i + 1 == block.values.size() && block.terminator.kind == TerminatorKind::Invoke) {
                    block.terminator.kind = TerminatorKind::Jump;
                    block.terminator.targets.shrink(1);
                }
            }
            for (auto& target : block.terminator.targets) {
                unsigned to = target.block.index();
                if (to == madeIn || !live[to])
                    continue;
                for (unsigned field = 0; field < fields.size(); ++field) {
                    if (!(live[to] & 1ull << field))
                        continue;
                    RELEASE_ASSERT(held[field]);
                    target.arguments.append(held[field]);
                }
            }
        }
        return true;
    }

    static bool isConstant(const Value& value)
    {
        switch (value.opcode) {
        case Opcode::Undefined:
        case Opcode::Null:
        case Opcode::Boolean:
        case Opcode::Integer:
        case Opcode::Double:
        case Opcode::String:
            return true;
        default:
            return false;
        }
    }

    // `const o = new T(..); n++; o.id = n` where the callee is a prediction: the arm that makes the object and the general arm come together, and what the function does next stands behind the join. Up in the arm it follows the allocation.
    // The head of the block, up to the last store into the object before anything else gets to see it, is carried out at the end of every arm instead: once on every way, in the same order.
    void moveStoresIntoArms()
    {
        static constexpr size_t maxValuesCopiedIntoArms = 40;
        struct WayIn {
            unsigned block;
            bool isJump;
        };
        Vector<Vector<WayIn, 2>> waysIn(m_function.blocks.size());
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            auto& terminator = m_function.blocks[b].terminator;
            for (auto& target : terminator.targets)
                waysIn[target.block.index()].append(WayIn { b, terminator.kind == TerminatorKind::Jump && target.block.index() != b });
        }
        auto isMadeIn = [&](ValueID value, unsigned block) {
            while (m_function.at(value).opcode == Opcode::Cast && m_function.at(value).castReason() == CastReason::Upcast)
                value = m_function.at(value).child(0);
            return m_function.at(value).opcode == Opcode::NewObjectWithShape && m_function.blocks[block].values.contains(value);
        };
        auto appendTo = [&](unsigned block, Value&& value) {
            ValueID id(m_function.values.size());
            m_function.values.append(WTF::move(value));
            m_function.blocks[block].values.append(id);
            return id;
        };
        auto copyIntoArms = [&](unsigned j, size_t end, const Vector<WayIn, 2>& arms) {
            Vector<ValueID, 16> head(m_function.blocks[j].values.span().first(end));
            Vector<ValueID, 4> parameters(m_function.blocks[j].parameters.span());
            BitVector isInHead(m_function.values.size());
            for (ValueID id : head)
                isInHead.set(id.index());
            BitVector isUsedFurtherOn(m_function.values.size());
            auto use = [&](ValueID value) {
                if (isInHead.get(value.index()))
                    isUsedFurtherOn.set(value.index());
            };
            for (auto& block : m_function.blocks) {
                for (ValueID id : block.values) {
                    if (!isInHead.get(id.index()))
                        std::as_const(m_function.at(id)).forEachChild(use);
                }
                block.terminator.forEachChild([&](ValueID& value) {
                    use(value);
                });
            }
            for (ValueID id : head) {
                if (isUsedFurtherOn.get(id.index()) && m_function.at(id).rep == Rep::Void)
                    return;
            }
            for (auto& arm : arms) {
                Arguments given = m_function.blocks[arm.block].terminator.targets[0].arguments;
                Vector<std::pair<ValueID, ValueID>, 16> copies;
                // A parameter is what the arm passes for it, seen as the same type as before: whatever goes by the type means what it meant.
                auto copyOf = [&](ValueID value) {
                    for (auto& [original, copy] : copies) {
                        if (original == value)
                            return copy;
                    }
                    size_t position = parameters.find(value);
                    if (position == notFound)
                        return value;
                    ValueID passed = given[position];
                    if (m_function.at(passed).type != m_function.at(value).type || m_function.at(value).hasPredictionUnlike(m_function.at(passed))) {
                        Value upcast = m_function.at(value);
                        upcast.opcode = Opcode::Cast;
                        upcast.children = { passed };
                        upcast.immediates = { Format::none, static_cast<uint32_t>(CastReason::Upcast) };
                        passed = appendTo(arm.block, WTF::move(upcast));
                    }
                    copies.append({ value, passed });
                    return passed;
                };
                for (ValueID id : head) {
                    Value copy = m_function.at(id);
                    copy.forEachChild([&](ValueID& child) {
                        child = copyOf(child);
                    });
                    ValueID made = appendTo(arm.block, WTF::move(copy));
                    copies.append({ id, made });
                    if (isUsedFurtherOn.get(id.index()))
                        m_function.blocks[arm.block].terminator.targets[0].arguments.append(made);
                }
            }
            auto& join = m_function.blocks[j];
            join.values.removeAt(0, end);
            for (ValueID id : head) {
                if (!isUsedFurtherOn.get(id.index()))
                    continue;
                auto& original = m_function.at(id);
                Value parameter;
                parameter.opcode = Opcode::Parameter;
                parameter.position = original.position;
                parameter.inlineFrame = original.inlineFrame;
                parameter.type = original.type;
                parameter.rep = original.rep;
                parameter.takePredictionOf(original);
                parameter.immediates = { j, static_cast<uint32_t>(join.parameters.size()) };
                original = WTF::move(parameter);
                join.parameters.append(id);
            }
        };
        for (unsigned j = 0; j < m_function.blocks.size(); ++j) {
            auto& arms = waysIn[j];
            bool areJumps = arms.size() >= 2;
            for (auto& arm : arms)
                areJumps &= arm.isJump;
            if (!areJumps)
                continue;
            if (m_movesBirths) {
                // Upcasts of parameters, the birth, its stores: nothing of it is made in the block but out of those.
                auto& join = m_function.blocks[j];
                Vector<ValueID, 8> given(join.parameters.span());
                BitVector isMadeHere(m_function.values.size());
                for (ValueID id : join.values)
                    isMadeHere.set(id.index());
                auto isAtHand = [&](ValueID operand) {
                    return given.contains(operand) || !isMadeHere.get(operand.index());
                };
                ValueID born;
                size_t end = 0;
                for (size_t at = 0; at < std::min<size_t>(join.values.size(), maxValuesCopiedIntoArms); ++at) {
                    ValueID id = join.values[at];
                    auto& value = m_function.at(id);
                    bool areAtHand = true;
                    value.forEachChild([&](ValueID child) {
                        areAtHand &= child == born || isAtHand(child);
                    });
                    if (!areAtHand)
                        break;
                    if (!born && (isConstant(value) || (value.opcode == Opcode::Cast && value.castReason() == CastReason::Upcast))) {
                        given.append(id);
                        continue;
                    }
                    bool isOfWrapper = value.inlineFrame && m_program.at(m_function.inlineFrames[value.inlineFrame].callee).worker;
                    if (!born && isOfWrapper && (value.opcode == Opcode::NewArray || (value.opcode == Opcode::NewObjectWithShape && !value.child(0)))) {
                        born = id;
                        end = at + 1;
                        continue;
                    }
                    if (!born || value.opcode != Opcode::PutField || value.child(0) != born || value.child(1) == born)
                        break;
                    end = at + 1;
                }
                if (born)
                    copyIntoArms(j, end, arms);
                continue;
            }
            for (size_t position = 0; position < m_function.blocks[j].parameters.size(); ++position) {
                bool isMadeInAnArm = false;
                for (auto& arm : arms)
                    isMadeInAnArm |= isMadeIn(m_function.blocks[arm.block].terminator.targets[0].arguments[position], arm.block);
                if (!isMadeInAnArm)
                    continue;
                auto& join = m_function.blocks[j];
                Vector<ValueID, 2> seenAs { join.parameters[position] };
                size_t limit = std::min<size_t>(join.values.size() - (join.terminator.kind == TerminatorKind::Invoke), maxValuesCopiedIntoArms);
                size_t end = 0;
                for (size_t at = 0; at < limit; ++at) {
                    ValueID id = join.values[at];
                    auto& value = m_function.at(id);
                    if (value.opcode == Opcode::LoopHint || value.opcode == Opcode::CatchValue || value.opcode == Opcode::Await || value.opcode == Opcode::Yield || value.opcode == Opcode::GetResumeMode)
                        break;
                    if (value.opcode == Opcode::Cast && value.castReason() == CastReason::Upcast && seenAs.contains(value.child(0))) {
                        seenAs.append(id);
                        continue;
                    }
                    bool usesObject = false;
                    value.forEachChild([&](ValueID child) {
                        usesObject |= seenAs.contains(child);
                    });
                    if (!usesObject)
                        continue;
                    if (value.opcode != Opcode::PutById || value.isInitialStore() || !seenAs.contains(value.child(0)) || seenAs.contains(value.child(1)))
                        break;
                    end = at + 1;
                }
                if (end)
                    copyIntoArms(j, end, arms);
            }
        }
    }

    // An allocation reads what the `prototype` of the constructor is, which is no field: only what may store anywhere may store that. Nothing else can tell when an object was made that nobody has seen.
    bool mayGoAboveAllocation(const Block& block, size_t index) const
    {
        switch (m_function.at(block.values[index]).opcode) {
        case Opcode::CallDirect:
        case Opcode::Await:
        case Opcode::Yield:
        case Opcode::GetResumeMode:
        case Opcode::LoopHint:
        case Opcode::CatchValue:
            return false;
        default:
            return writesOf(m_program, m_function, block, index) <= Writes::Fields;
        }
    }

    bool mayStoreToObjectItself(ValueID object, const Value& store) const
    {
        LayoutID made = m_function.at(object).layout();
        auto& layout = m_program.at(made);
        auto field = layout.fieldNamed(store.string());
        if (!isObjectLayout(layout.kind) || !field || mayInheritAtBirth(m_program, m_function, object, store.string()))
            return false;
        TypeID type = layout.fields[*field].type;
        return !type || m_program.typeOf(m_function, store.child(1)).isWithin(m_program.at(type), m_program);
    }

    // The stores that follow an allocation with nothing in between make the object what it is born as: isInitializingStore(). A constructor
    // that was copied in has what it stores in between, and stores through the cast that `this` was bound by.
    void gatherInitializingStores()
    {
        // By allocation: how many stores into it, straight or through an upcast of it, its block has; and how many of them are initial stores through an upcast.
        Vector<uint32_t> numberOfStores;
        Vector<uint32_t> numberOfInitialStoresThroughUpcasts;
        for (auto& block : m_function.blocks) {
            bool endsInInvoke = block.terminator.kind == TerminatorKind::Invoke;
            for (ValueID id : block.values) {
                auto& value = m_function.at(id);
                if (value.opcode != Opcode::PutField && value.opcode != Opcode::PutById)
                    continue;
                ValueID into = value.child(0);
                bool isThroughUpcast = m_function.at(into).opcode == Opcode::Cast && m_function.at(into).castReason() == CastReason::Upcast;
                if (isThroughUpcast)
                    into = m_function.at(into).child(0);
                if (m_function.at(into).opcode != Opcode::NewObjectWithShape)
                    continue;
                if (numberOfStores.isEmpty()) {
                    numberOfStores.fill(0, m_function.values.size());
                    numberOfInitialStoresThroughUpcasts.fill(0, m_function.values.size());
                }
                ++numberOfStores[into.index()];
                if (isThroughUpcast && value.opcode == Opcode::PutById && value.isInitialStore())
                    ++numberOfInitialStoresThroughUpcasts[into.index()];
            }
            if (numberOfStores.isEmpty())
                continue;
            for (size_t i = 0; i < block.values.size(); ++i) {
                ValueID object = block.values[i];
                if (m_function.at(object).opcode != Opcode::NewObjectWithShape || !numberOfStores[object.index()])
                    continue;
                auto isUpcastOfObject = [&](ValueID id) {
                    auto& value = m_function.at(id);
                    return value.opcode == Opcode::Cast && value.castReason() == CastReason::Upcast && value.child(0) == object;
                };
                bool initialStoresMayGoOver = true;
                uint32_t left = numberOfInitialStoresThroughUpcasts[object.index()];
                for (size_t next = i + 1; left && next < block.values.size(); ++next) {
                    auto& value = m_function.at(block.values[next]);
                    if (value.opcode == Opcode::PutById && value.isInitialStore() && isUpcastOfObject(value.child(0))) {
                        initialStoresMayGoOver &= mayStoreToObjectItself(object, value);
                        --left;
                    }
                }
                left = numberOfInitialStoresThroughUpcasts[object.index()];
                for (size_t next = i + 1; initialStoresMayGoOver && left && next < block.values.size(); ++next) {
                    auto& value = m_function.at(block.values[next]);
                    if (value.opcode == Opcode::PutById && value.isInitialStore() && isUpcastOfObject(value.child(0))) {
                        value.children[0] = object;
                        --left;
                    }
                }
                // How far: up to the last plain store into the object before anything else uses it, or something comes that has to stay behind the allocation.
                Vector<ValueID, 2> upcasts;
                size_t lastStore = i;
                size_t limit = block.values.size() - endsInInvoke;
                left = numberOfStores[object.index()];
                for (size_t next = i + 1; left && next < limit; ++next) {
                    ValueID id = block.values[next];
                    auto& value = m_function.at(id);
                    if (isUpcastOfObject(id)) {
                        upcasts.append(id);
                        continue;
                    }
                    if ((value.opcode == Opcode::PutField || value.opcode == Opcode::PutById) && (value.child(0) == object || upcasts.contains(value.child(0))))
                        --left;
                    if (value.opcode == Opcode::PutById && !value.isInitialStore() && upcasts.contains(value.child(0)) && mayStoreToObjectItself(object, value))
                        value.children[0] = object;
                    bool usesObject = false;
                    value.forEachChild([&](ValueID child) {
                        usesObject |= child == object || upcasts.contains(child);
                    });
                    if (!usesObject) {
                        if (!mayGoAboveAllocation(block, next))
                            break;
                        continue;
                    }
                    if ((value.opcode != Opcode::PutField && value.opcode != Opcode::PutById) || value.child(0) != object || value.child(1) == object || upcasts.contains(value.child(1)))
                        break;
                    // It may run a setter, or throw.
                    if (value.opcode == Opcode::PutById && !mayStoreToObjectItself(object, value))
                        break;
                    lastStore = next;
                }
                if (lastStore == i)
                    continue;
                Vector<ValueID, 16> above;
                Vector<ValueID, 16> stores;
                Vector<ValueID, 2> behind;
                for (size_t next = i + 1; next <= lastStore; ++next) {
                    ValueID id = block.values[next];
                    if (isUpcastOfObject(id))
                        behind.append(id);
                    else if (m_function.at(id).child(0) == object && (m_function.at(id).opcode == Opcode::PutField || m_function.at(id).opcode == Opcode::PutById))
                        stores.append(id);
                    else
                        above.append(id);
                }
                size_t at = i;
                for (ValueID id : above)
                    block.values[at++] = id;
                i = at;
                block.values[at++] = object;
                for (ValueID id : stores)
                    block.values[at++] = id;
                for (ValueID id : behind)
                    block.values[at++] = id;
            }
        }
    }

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

    // `typeof x === "function"`: the string is not made.
    void foldTypeOfComparisons()
    {
        auto definitionOf = [&](ValueID id) -> const Value& {
            while (m_function.at(id).opcode == Opcode::Cast && m_function.at(id).castReason() == CastReason::Upcast)
                id = m_function.at(id).child(0);
            return m_function.at(id);
        };
        for (auto& block : m_function.blocks) {
            for (ValueID id : block.values) {
                auto& value = m_function.at(id);
                if (value.opcode != Opcode::StrictEq && value.opcode != Opcode::Eq)
                    continue;
                // What a handler watches stays what it is.
                if (block.terminator.kind == TerminatorKind::Invoke && id == block.values.last())
                    continue;
                for (unsigned i = 0; i < 2; ++i) {
                    auto& typeOf = definitionOf(value.child(i));
                    auto& string = definitionOf(value.child(1 - i));
                    if (typeOf.opcode != Opcode::TypeOf || string.opcode != Opcode::String)
                        continue;
                    ValueID operand = typeOf.child(0);
                    auto wanted = typeofTypeNamed(m_program.string(string.string()));
                    value.opcode = wanted ? Opcode::TypeOfIs : Opcode::Boolean;
                    value.children = { };
                    if (wanted)
                        value.children[0] = operand;
                    value.immediates = { wanted ? static_cast<uint32_t>(*wanted) : 0, Format::none };
                    break;
                }
            }
        }
    }

    // `super(a, b)`: the front end makes an array of the arguments for the operation. Where that is a literal that nothing else uses the
    // arguments are the operands, and the array goes as any value that is not used. `super(...[a], b)`: b is pushed to what the spread has become.
    void foldSuperConstructs()
    {
        Vector<unsigned> uses;
        for (auto& value : m_function.values) {
            if (languageOperationOf(m_program, value) != LanguageOperation::SuperConstruct || value.arguments.size() != 3)
                continue;
            auto& list = m_function.at(value.arguments[2]);
            if (list.opcode != Opcode::NewArray || mayThrow(m_program, m_function, list))
                continue;
            if (uses.isEmpty()) {
                uses.fill(0, m_function.values.size());
                for (auto& block : m_function.blocks) {
                    for (ValueID id : block.values) {
                        std::as_const(m_function.at(id)).forEachChild([&](ValueID child) {
                            ++uses[child.index()];
                        });
                    }
                    block.terminator.forEachChild([&](ValueID& child) {
                        ++uses[child.index()];
                    });
                }
            }
            if (uses[value.arguments[2].index()] != 1)
                continue;
            Arguments elements = list.arguments;
            value.opcode = Opcode::ConstructSuper;
            value.children = { value.arguments[0], value.arguments[1], ValueID() };
            value.immediates = { Format::none, Format::none };
            value.arguments = WTF::move(elements);
        }
    }

    // `f(...list)`: the front end makes an empty array, appends what the list yields, and applies the function to the array. Nothing comes
    // between the last two but the edge to a handler.
    void foldSpreadCalls()
    {
        BuiltinID callWithSpread = Builtins::singleton().find(BuiltinKind::Operation, { }, Builtins::singleton().nameOf("CallWithSpread"_s.span8()));
        Vector<unsigned> uses;
        Vector<unsigned> waysIn;
        Vector<BlockID> comesFrom;
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            auto& block = m_function.blocks[b];
            for (unsigned i = 0; i < block.values.size(); ++i) {
                auto& apply = m_function.at(block.values[i]);
                if (languageOperationOf(m_program, apply) != LanguageOperation::Apply || apply.arguments.size() != 3)
                    continue;
                ValueID appended = apply.arguments[2];
                auto& append = m_function.at(appended);
                if (languageOperationOf(m_program, append) != LanguageOperation::AppendSpread || append.arguments.size() != 2)
                    continue;
                auto& target = m_function.at(append.arguments[0]);
                if (target.opcode != Opcode::NewArray || !target.arguments.isEmpty())
                    continue;
                if (uses.isEmpty()) {
                    uses.fill(0, m_function.values.size());
                    waysIn.fill(0, m_function.blocks.size());
                    comesFrom.fill(BlockID(), m_function.blocks.size());
                    for (unsigned from = 0; from < m_function.blocks.size(); ++from) {
                        auto& other = m_function.blocks[from];
                        for (ValueID id : other.values) {
                            std::as_const(m_function.at(id)).forEachChild([&](ValueID child) {
                                ++uses[child.index()];
                            });
                        }
                        other.terminator.forEachChild([&](ValueID& value) {
                            ++uses[value.index()];
                        });
                        for (auto& to : other.terminator.targets) {
                            ++waysIn[to.block.index()];
                            comesFrom[to.block.index()] = BlockID(from);
                        }
                    }
                }
                if (uses[appended.index()] != 1 || uses[append.arguments[0].index()] != 1)
                    continue;
                auto& before = i ? block : m_function.at(waysIn[b] == 1 ? comesFrom[b] : BlockID(b));
                bool isWatched = !i && before.terminator.kind == TerminatorKind::Invoke && before.terminator.targets[0].block == BlockID(b) && !before.values.isEmpty();
                if ((i ? before.values[i - 1] : isWatched ? before.values.last() : ValueID()) != appended)
                    continue;
                apply.opcode = Opcode::CallBuiltin;
                apply.immediates = { callWithSpread.index(), Format::none };
                apply.arguments[2] = append.arguments[1];
                if (isWatched) {
                    before.terminator.kind = TerminatorKind::Jump;
                    before.terminator.targets.shrink(1);
                    before.values.removeLast();
                } else
                    before.values.removeAt(--i);
            }
        }
    }

    // What cannot throw needs no handler.
    void foldInvokes()
    {
        Vector<unsigned> onlyPredecessor;
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            auto& block = m_function.blocks[b];
            if (block.terminator.kind != TerminatorKind::Invoke)
                continue;
            if (!block.values.isEmpty() && mayThrow(m_program, m_function, m_function.at(block.values.last()))) {
                if (m_function.at(block.values.last()).opcode != Opcode::PutById)
                    continue;
                if (onlyPredecessor.isEmpty())
                    onlyPredecessor = findOnlyPredecessors();
                if (!endsInStoreToUnseenObject(b, onlyPredecessor))
                    continue;
            }
            block.terminator.kind = TerminatorKind::Jump;
            block.terminator.targets.shrink(1);
        }
    }

    static constexpr unsigned noBlock = UINT_MAX;

    // By block: the block it is entered from, if there is one way in.
    Vector<unsigned> findOnlyPredecessors() const
    {
        static constexpr unsigned several = noBlock - 1;
        Vector<unsigned> result;
        result.fill(noBlock, m_function.blocks.size());
        result[0] = several;
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            for (auto& target : m_function.blocks[b].terminator.targets) {
                unsigned& from = result[target.block.index()];
                from = from == noBlock ? b : several;
            }
        }
        for (unsigned& from : result) {
            if (from == several)
                from = noBlock;
        }
        return result;
    }

    // The last value of the block is a plain store into an object that was made in this function and that nobody has seen since: it is as it
    // was made. Such a store runs no code and cannot throw.
    bool endsInStoreToUnseenObject(unsigned block, const Vector<unsigned>& onlyPredecessor) const
    {
        auto withoutUpcasts = [&](ValueID id) {
            while (id && m_function.at(id).opcode == Opcode::Cast && m_function.at(id).castReason() == CastReason::Upcast)
                id = m_function.at(id).child(0);
            return id;
        };
        ValueID object = withoutUpcasts(m_function.at(m_function.blocks[block].values.last()).child(0));
        if (m_function.at(object).opcode != Opcode::NewObjectWithShape)
            return false;
        auto leavesObjectUnseen = [&](const Value& value) {
            bool isOperand = false;
            value.forEachChild([&](ValueID child) {
                isOperand |= withoutUpcasts(child) == object;
            });
            if (!isOperand || (value.opcode == Opcode::Cast && value.castReason() == CastReason::Upcast))
                return true;
            if (withoutUpcasts(value.child(0)) != object || withoutUpcasts(value.child(1)) == object)
                return false;
            return value.opcode == Opcode::PutField || (value.opcode == Opcode::PutById && mayStoreToObjectItself(object, value));
        };
        unsigned at = block;
        for (unsigned steps = 0; steps < m_function.blocks.size(); ++steps) {
            auto& values = m_function.blocks[at].values;
            for (size_t i = values.size(); i--;) {
                if (values[i] == object)
                    return true;
                if (!leavesObjectUnseen(m_function.at(values[i])))
                    return false;
            }
            at = onlyPredecessor[at];
            if (at == noBlock)
                return false;
            // Into a block parameter: it has another name from there on.
            bool flowsOn = false;
            Terminator terminator = m_function.blocks[at].terminator;
            terminator.forEachChild([&](ValueID& child) {
                flowsOn |= withoutUpcasts(child) == object;
            });
            if (flowsOn)
                return false;
        }
        return false;
    }

    // After inlining: `type === "error"` where `type` is "x". What depends on it goes before the escape analysis looks.
    void foldConstantConditions()
    {
        auto definitionOf = [&](ValueID id) -> const Value& {
            while (m_function.at(id).opcode == Opcode::Cast && m_function.at(id).castReason() == CastReason::Upcast)
                id = m_function.at(id).child(0);
            return m_function.at(id);
        };
        auto isNever = [&](ValueID id, uint16_t primitives) {
            while (m_function.at(id).opcode == Opcode::Cast && m_function.at(id).castReason() == CastReason::Upcast)
                id = m_function.at(id).child(0);
            return !(m_program.typeOf(m_function, id).primitives & primitives);
        };
        for (auto& block : m_function.blocks) {
            for (ValueID id : block.values) {
                auto& value = m_function.at(id);
                // What a handler watches stays what it is.
                if (block.terminator.kind == TerminatorKind::Invoke && id == block.values.last())
                    continue;
                std::optional<bool> result;
                switch (value.opcode) {
                case Opcode::StrictEq:
                case Opcode::Eq:
                    result = areEqualConstants(m_program, definitionOf(value.child(0)), definitionOf(value.child(1)), value.opcode == Opcode::StrictEq);
                    for (unsigned side = 0; side < 2 && !result && value.opcode == Opcode::StrictEq; ++side) {
                        Opcode constant = definitionOf(value.child(side)).opcode;
                        if ((constant == Opcode::Undefined && isNever(value.child(1 - side), Primitive::Undefined)) || (constant == Opcode::Null && isNever(value.child(1 - side), Primitive::Null)))
                            result = false;
                    }
                    break;
                case Opcode::LogicalNot:
                    if (auto& operand = definitionOf(value.child(0)); operand.opcode == Opcode::Boolean)
                        result = !operand.boolean();
                    break;
                case Opcode::ToBoolean:
                    if (auto& operand = definitionOf(value.child(0)); operand.opcode == Opcode::Boolean)
                        result = operand.boolean();
                    else if (operand.opcode == Opcode::Integer)
                        result = !!operand.integer();
                    else if (operand.opcode == Opcode::Undefined || operand.opcode == Opcode::Null)
                        result = false;
                    break;
                case Opcode::IsUndefinedOrNull:
                    if (auto& operand = definitionOf(value.child(0)); operand.opcode == Opcode::Undefined || operand.opcode == Opcode::Null)
                        result = true;
                    else if (isNever(value.child(0), Primitive::UndefinedOrNull))
                        result = false;
                    break;
                case Opcode::TypeOfIs:
                    if (auto& operand = definitionOf(value.child(0)); operand.opcode == Opcode::NewClosure || operand.opcode == Opcode::NewClosureOnce || operand.opcode == Opcode::FunctionReference)
                        result = value.typeofType() == TypeofType::Function;
                    break;
                case Opcode::IsClosureOf:
                    if (auto& operand = definitionOf(value.child(0)); operand.opcode == Opcode::NewClosure || operand.opcode == Opcode::NewClosureOnce || operand.opcode == Opcode::FunctionReference) {
                        // Whether it is still as it was made is not known from where it was made.
                        if (operand.function() != value.function())
                            result = false;
                        else if (value.immediates[1] != closureIsAsMade)
                            result = true;
                    }
                    break;
                default:
                    break;
                }
                if (!result)
                    continue;
                value.opcode = Opcode::Boolean;
                value.children = { };
                value.immediates = { *result, Format::none };
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
        case Opcode::GetArgument:
        case Opcode::NewArray:
            return !mayThrow(m_program, m_function, value);
        case Opcode::Operation:
        case Opcode::CallBuiltin:
            return languageOperationOf(m_program, value) == LanguageOperation::CreateArguments;
        case Opcode::Undefined:
        case Opcode::Null:
        case Opcode::Boolean:
        case Opcode::Integer:
        case Opcode::Double:
        case Opcode::String:
        case Opcode::BigInt:
        case Opcode::FunctionReference:
        case Opcode::NewClosure:
        // That the variable is filled is nothing anyone can tell.
        case Opcode::NewClosureOnce:
        case Opcode::NewObjectWithShape:
        case Opcode::NewObject:
        case Opcode::GetCapture:
        case Opcode::CreateRest:
        case Opcode::GetRestLength:
        case Opcode::CreateThis:
        case Opcode::LogicalNot:
        case Opcode::IsUndefinedOrNull:
        case Opcode::HasShapeOf:
        case Opcode::IsClassObjectAsDeclared:
        case Opcode::IsClosureOf:
        case Opcode::IsBuiltinFunction:
        case Opcode::BuiltinFunction:
        case Opcode::TypeOf:
        case Opcode::TypeOfIs:
            return true;
        case Opcode::Cast:
            return value.castReason() == CastReason::Upcast;
        default:
            return false;
        }
    }

    void discountAllocationAssertions(Vector<unsigned>& uses)
    {
        BitVector isPlaced(m_function.values.size());
        for (auto& block : m_function.blocks) {
            for (ValueID id : block.parameters)
                isPlaced.quickSet(id.index());
            for (ValueID id : block.values)
                isPlaced.quickSet(id.index());
        }
        for (auto& block : m_function.blocks) {
            block.values.removeAllMatching([&](ValueID id) {
                auto& value = m_function.at(id);
                if (value.opcode != Opcode::AssertNotAllocated)
                    return false;
                for (auto* operand = &m_function.at(value.child(0)); operand->opcode == Opcode::Cast && operand->castReason() != CastReason::As && operand->castReason() != CastReason::Coerce; operand = &m_function.at(value.child(0)))
                    value.children[0] = operand->child(0);
                if (!isPlaced.quickGet(value.child(0).index()))
                    return true;
                --uses[value.child(0).index()];
                return false;
            });
        }
    }

    ValueID behindUpcasts(ValueID id) const
    {
        while (m_function.at(id).opcode == Opcode::Cast && m_function.at(id).castReason() == CastReason::Upcast)
            id = m_function.at(id).child(0);
        return id;
    }

    struct StoreToOwnObject {
        ValueID object;
        ValueID stored;
    };

    // It fills an object that the function has made. As long as nobody has seen the object that is all it does: it runs no code and cannot throw.
    std::optional<StoreToOwnObject> storeToOwnObject(const Value& value) const
    {
        switch (value.opcode) {
        case Opcode::PutField:
        case Opcode::PutById: {
            ValueID object = behindUpcasts(value.child(0));
            switch (m_function.at(object).opcode) {
            case Opcode::NewObjectWithShape:
                if (value.opcode == Opcode::PutById && !mayStoreToObjectItself(object, value))
                    return std::nullopt;
                break;
            // Nothing can be added to Object.prototype, and it has one setter.
            case Opcode::NewObject:
                if (value.opcode != Opcode::PutById || equalSpans(m_program.string(value.string()), "__proto__"_s.span8()))
                    return std::nullopt;
                break;
            default:
                return std::nullopt;
            }
            return StoreToOwnObject { object, value.child(1) };
        }
        case Opcode::Operation:
        case Opcode::CallBuiltin: {
            if (languageOperationOf(m_program, value) != LanguageOperation::DefineField || value.arguments.size() != 3)
                return std::nullopt;
            ValueID object = behindUpcasts(value.arguments[0]);
            if (m_function.at(object).opcode != Opcode::NewObject || m_function.at(value.arguments[1]).opcode != Opcode::String)
                return std::nullopt;
            return StoreToOwnObject { object, value.arguments[2] };
        }
        default:
            return std::nullopt;
        }
    }

    // Nobody can tell whether an object that is only stored to has been made. What only such an object holds, nobody holds.
    bool removeObjectsOnlyStoredTo()
    {
        BitVector isOwnObject(m_function.values.size());
        bool hasOwnObjects = false;
        for (auto& block : m_function.blocks) {
            for (ValueID id : block.values) {
                Opcode opcode = m_function.at(id).opcode;
                if (opcode != Opcode::NewObjectWithShape && opcode != Opcode::NewObject)
                    continue;
                isOwnObject.quickSet(id.index());
                hasOwnObjects = true;
            }
        }
        if (!hasOwnObjects)
            return false;

        // The last value of a block that a handler watches is what may throw.
        auto watchedIn = [](const Block& block) {
            return block.terminator.kind == TerminatorKind::Invoke && !block.values.isEmpty() ? block.values.last() : ValueID();
        };
        auto isUpcast = [](const Value& value) {
            return value.opcode == Opcode::Cast && value.castReason() == CastReason::Upcast;
        };
        BitVector isNeeded(m_function.values.size());
        auto need = [&](ValueID id) {
            isNeeded.quickSet(behindUpcasts(id).index());
        };
        Vector<StoreToOwnObject, 8> held;
        for (auto& block : m_function.blocks) {
            ValueID watched = watchedIn(block);
            for (ValueID id : block.values) {
                auto& value = std::as_const(m_function).at(id);
                if (id == watched)
                    need(id);
                else if (value.opcode == Opcode::AssertNotAllocated || isUpcast(value))
                    continue;
                else if (auto store = storeToOwnObject(value)) {
                    ValueID stored = behindUpcasts(store->stored);
                    if (isOwnObject.quickGet(stored.index()))
                        held.append({ store->object, stored });
                    continue;
                }
                value.forEachChild(need);
            }
            block.terminator.forEachChild([&](ValueID& id) {
                need(id);
            });
        }
        for (bool changed = true; changed;) {
            changed = false;
            for (auto& [object, stored] : held) {
                if (isNeeded.quickGet(object.index()) && !isNeeded.quickSet(stored.index()))
                    changed = true;
            }
        }

        auto isRemoved = [&](ValueID object) {
            return isOwnObject.quickGet(object.index()) && !isNeeded.quickGet(object.index());
        };
        bool any = false;
        for (auto& block : m_function.blocks) {
            ValueID watched = watchedIn(block);
            any |= !!block.values.removeAllMatching([&](ValueID id) {
                if (id == watched)
                    return false;
                auto& value = m_function.at(id);
                if (isUpcast(value) || isOwnObject.quickGet(id.index()))
                    return isRemoved(behindUpcasts(id));
                auto store = storeToOwnObject(value);
                return store && isRemoved(store->object);
            });
        }
        return any;
    }

    bool removeUnusedValues()
    {
        bool any = removeObjectsOnlyStoredTo();
        Vector<unsigned> uses;
        uses.fill(0, m_function.values.size());
        auto use = [&](ValueID value) {
            ++uses[value.index()];
        };
        if (m_program.hasAllocationAssertions) [[unlikely]]
            discountAllocationAssertions(uses);
        for (auto& block : m_function.blocks) {
            for (ValueID id : block.values)
                std::as_const(m_function.at(id)).forEachChild(use);
            block.terminator.forEachChild([&](ValueID& value) {
                use(value);
            });
        }
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
    bool m_movesBirths { false };
};

} // anonymous namespace

bool eliminateRedundantReads(const Program& program, Function& function)
{
    return RedundantReadElimination(program, function).run();
}

void moveBirthsIntoArms(const Program& program, Function& function)
{
    Cleanup(program, function).moveBirthsIntoArms();
}

void cleanUp(const Program& program, Function& function)
{
    noteAllocationSites(program, function);
    Cleanup(program, function).run();
}

std::optional<bool> areEqualConstants(const Program& program, const Value& a, const Value& b, bool isStrict)
{
    auto isNil = [](const Value& value) {
        return value.opcode == Opcode::Undefined || value.opcode == Opcode::Null;
    };
    auto numberOf = [](const Value& value) -> std::optional<double> {
        if (value.opcode == Opcode::Integer)
            return static_cast<double>(value.integer());
        if (value.opcode == Opcode::Double)
            return std::bit_cast<double>(value.bits());
        return std::nullopt;
    };
    auto isConstant = [&](const Value& value) {
        return isNil(value) || value.opcode == Opcode::Boolean || value.opcode == Opcode::String || numberOf(value);
    };
    if (!isConstant(a) || !isConstant(b))
        return std::nullopt;
    if (isNil(a) || isNil(b))
        return isStrict ? a.opcode == b.opcode : isNil(a) && isNil(b);
    if (numberOf(a) && numberOf(b))
        return *numberOf(a) == *numberOf(b);
    if (a.opcode != b.opcode)
        return isStrict ? std::optional(false) : std::nullopt;
    if (a.opcode == Opcode::Boolean)
        return a.boolean() == b.boolean();
    return equalSpans(program.string(a.string()), program.string(b.string()));
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
    case Opcode::NewClosureOnce:
    case Opcode::GetCapture:
    case Opcode::CreateRest:
    case Opcode::GetRestLength:
    case Opcode::CreateThis:
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
    case Opcode::IsClassObjectAsDeclared:
    case Opcode::IsClosureOf:
    case Opcode::IsBuiltinFunction:
    case Opcode::BuiltinFunction:
    case Opcode::TypeOfIs:
    case Opcode::AssertNotAllocated:
    case Opcode::CatchValue:
    case Opcode::LoopHint:
    case Opcode::GetResumeMode:
        return false;
    case Opcode::IsInstanceOfClass:
        return mayWalkPrototypeChain(program, function, value);
    case Opcode::NewArray:
        return !Type::engineClass(Builtins::singleton().arrayClass()).isWithin(program.at(value.type), program);
    case Opcode::GetArgument:
        return !program.at(program.types.anything).isWithin(program.at(value.type), program);
    case Opcode::GetMyArgumentByVal:
        return program.typeOf(function, value.child(0)).hasObjects() || !program.at(program.types.anything).isWithin(program.at(value.type), program);
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
        // What it holds its operand to is not its type then.
        if (Options::auditAOTTypedAssertions()) [[unlikely]]
            return true;
        auto& operand = program.typeOf(function, value.child(0));
        // A conversion may throw, and so may whatever goes through a plain object, for want of room.
        if (value.castReason() == CastReason::Coerce)
            return !operand.isWithin(program.at(value.type), program) || coercionMayChangeObjects(program, operand, program.at(value.coercedType()), value.coercesFreshData());
        if (value.castReason() == CastReason::NonNull && operand.mayBeUndefinedOrNull())
            return true;
        // With the name of a property it stands for the access, which turns away `undefined` and `null` whatever the type says.
        if (value.accessedName())
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
        if (!(program.at(value.type).primitives & Primitive::Number))
            return true;
        [[fallthrough]];
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

// The iterator of an array has no `return` method, nor has that of a string, and the prototypes of the engine stay as they are: closing it does nothing.
static bool closesIteratorOfArrayOrString(const Program& program, const Function& function, const Value& value)
{
    if (value.opcode != Opcode::IteratorClose)
        return false;
    if (keyedAccessOf(program, function, value) == KeyedAccess::Array)
        return true;
    auto& iterator = function.at(value.child(0));
    if (iterator.opcode != Opcode::IteratorOpen)
        return false;
    // For `undefined` and `null` there is no iterator: opening has thrown.
    auto& type = program.typeOf(function, iterator.child(0));
    return type.primitives & Primitive::String && type.hasOnlyPrimitives(Primitive::String | Primitive::UndefinedOrNull);
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
            onlyRethrows &= closesIteratorOfArrayOrString(program, function, function.at(handler.values[i]));
        // What unwinds to it unwinds to where it rethrows to.
        Vector<Target, 2> outer = handler.terminator.targets;
        // Nor does anything else look at what it caught: it dominates where it rethrows to.
        auto noteUse = [&](ValueID used) {
            onlyRethrows &= !handler.values.contains(used);
        };
        for (unsigned other = 0; other < function.blocks.size(); ++other) {
            if (other == b)
                continue;
            for (ValueID id : function.blocks[other].values)
                function.at(id).forEachChild(noteUse);
            function.blocks[other].terminator.forEachChild(noteUse);
        }
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
