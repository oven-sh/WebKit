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

namespace {

// Finds the function that is called, where that takes no more than looking it up.
//
// The receiver of a call by name, of a read or of a store is looked at as one type of the program, which is a fact: the object was
// allocated with one of the members of that layout. Each member says what it has by the name. One function: it is called. A few:
//
//     HasShapeOf object, layout, 0    whether the object was allocated with a member of the layout
//     HasShapeOf object, layout, 1    whether it was allocated with the layout itself
//
// tell which, and the last of them needs no test. `undefined` and `null` are turned away first, by a cast to the type of the layout
// that names the property.
//
// What is called or constructed is a function value. Which function is known where the operand is defined as one: FunctionReference,
// NewClosure, the read of a variable or a field of which the checker says that only closures of one function are stored to it
// (Variable::onlyFunction, Field::onlyFunction). Nothing is followed any further than that, and nothing is found out here.
//
//     IsClassObjectAsDeclared class    whether the class object of the class, if it has been made, still finds what the classes declare under
//                                      the names of their static methods and accessors: see Typed::State::watchMembers()
//
// stands in front of what is looked up on a class object. Where it fails the operation is carried out as it stands, by name.
//
//     IsClosureOf callee, function    whether the value is a closure of the function
//
// is needed where what the checker says is no fact but what it expects: of the place, or of the operand itself
// (Value::predictedClosure). That may be wrong, so nothing goes by it but which function is tested for: where the test fails the call is
// made as it would have been. Where it passes it is a direct call, which can be inlined. Of a variable of which all that is open is whether it
// holds `undefined` or `null`, the test is for those.
//
// `f.call(thisArg, ..)` and `f.apply(thisArg, list)` are indirect calls of `f` with an explicit `this`. Function.prototype is immutable, so
// what `f` finds under the name is the built-in function unless `f` has a property in front of it.
//
//     IsBuiltinFunction method, entry    whether the value is the function that the entry of the table describes
//
// is the guard. Behind it is a Call of `f`, which is resolved like any other, or the operation Apply.
//
// The receiver is the class object of a class that is evaluated once, where it is defined as that: FunctionReference of the constructor.
// Or the search starts behind such a class: `super`. What the classes declare and the program is not seen to replace stays where it
// is, so it is looked up here.

// The ramp of an async function, if that is what a call of the function comes to: it is one, or it is bound to one. Not that of a generator: what goes wrong while the parameters of that are bound is thrown to the caller.
static bool isRampOfAsyncFunction(const Function& function)
{
    return function.startFunction || (function.resumeFunction && (function.declaredAs & (functionIsAsync | functionIsGenerator)) == functionIsAsync);
}

const Function* rampBehind(const Program& program, const Function& callee)
{
    if (isRampOfAsyncFunction(callee))
        return &callee;
    if (callee.kind != FunctionKind::Bound || callee.blocks.isEmpty() || callee.blocks[0].values.isEmpty())
        return nullptr;
    auto& call = callee.at(callee.blocks[0].values.last());
    return call.opcode == Opcode::CallDirect && isRampOfAsyncFunction(program.at(call.function())) ? &program.at(call.function()) : nullptr;
}

// Of what an async function hands back. Nothing: it is none.
TypeID typeOfPromiseOf(const Program& program, const Function& callee)
{
    auto* ramp = rampBehind(program, callee);
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

    // For createBoundFunctions(): what the sites of the function ask for, as Program::keyOfBoundFunction().
    void collectBoundFunctions(Vector<uint64_t>& wanted) const
    {
        for (auto& value : m_function.values) {
            if (auto known = calleeBoundBy(value))
                wanted.append(Program::keyOfBoundFunction(known->function, numberOfArgumentsBoundBy(value)));
        }
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
                    m_usingTakesObjects = m_program.find(asBytes("The value of a using declaration must be an object, null or undefined"_s.span8())).value_or(StringID());
                    m_usingTakesDisposables = m_program.find(asBytes("The value of a using declaration has no callable dispose method"_s.span8())).value_or(StringID());
                    m_symbol = m_program.find(asBytes("Symbol"_s.span8())).value_or(StringID());
                    m_dispose = m_program.find(asBytes("dispose"_s.span8())).value_or(StringID());
                    m_typeError = m_program.find(asBytes("TypeError"_s.span8())).value_or(StringID());
                    m_suppressedError = m_program.find(asBytes("SuppressedError"_s.span8())).value_or(StringID());
                    countUsesAndFindUnreachedBlocks();
                }
                if (!m_usingTakesObjects || !m_usingTakesDisposables || !m_symbol || !m_dispose || !m_typeError || !m_suppressedError)
                    return;
                expandDisposableStack(BlockID(b), id);
            }
        }
    }

    unsigned run(CleanUpTiming timing)
    {
        noteRefutedCallees();
        unsigned resolved = 0;
        // What comes after a value that was replaced is in a new block, which has its turn.
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
    // See Inlining::depthOf().
    static constexpr uint32_t depthThatIsNotInlined = 1 << 16;

    struct Case {
        // None: the member keeps the function in a field.
        FunctionID callee;
        Vector<LayoutID, 4> members;
        // What tells its members from those of the cases that come after it. The last has none.
        LayoutID test;
        bool isExact { false };
    };
    using Cases = Vector<Case, maxCallees>;

    struct KnownCallee {
        FunctionID function;
        // Where the operand is defined.
        ValueID definition;
        // What the closure captures, where the operand is defined by making it. Otherwise it is read from the closure: see capturesOf().
        Arguments captures;
        bool isCertain { true };
        // All the operand can be besides is `undefined` or `null`.
        bool isCertainUnlessNil { false };
        FunctionID other { };
    };

    using Refuted = std::pair<ValueID, FunctionID>;
    using RefutedShape = std::pair<ValueID, LayoutID>;

    enum class Yields : uint8_t {
        Result,
        Undefined,
    };

    const Type& typeOf(ValueID value) const { return m_program.typeOf(m_function, value); }
    bool isWithin(ValueID value, TypeID type) const { return !type || typeOf(value).isWithin(m_program.at(type), m_program); }

    ValueID originOf(ValueID value) const
    {
        while (m_function.at(value).opcode == Opcode::Cast)
            value = m_function.at(value).child(0);
        return value;
    }

    // Whether the value at the index was replaced: then the block ends before it.
    bool resolve(BlockID block, unsigned index)
    {
        switch (m_function.at(m_function.at(block).values[index]).opcode) {
        case Opcode::CallMethod:
            return resolveByShape(block, index, MethodKind::Method) || resolveOnClassObject(block, index) || resolveCallOfExpectedBuiltin(block, index) || resolveCallOrApply(block, index) || resolveBind(block, index) || resolveCallOfField(block, index);
        case Opcode::GetById:
            return resolveByShape(block, index, MethodKind::Getter) || resolveOnClassObject(block, index);
        case Opcode::PutById:
            return resolveByShape(block, index, MethodKind::Setter) || resolveOnClassObject(block, index);
        case Opcode::Call:
            return resolveCallOfExpectedMethod(block, index) || resolveCall(block, index) || resolveCallOfExpectedBuiltin(block, index) || resolveCallOfMethodRead(block, index) || resolveCallOrApply(block, index);
        case Opcode::Operation:
            return resolveConstruct(block, index) || resolveSuper(block, index) || resolveCallNoInline(block, index);
        case Opcode::Construct:
            return resolveConstructOfFunction(block, index) || resolveCallOfExpectedBuiltin(block, index);
        case Opcode::ConstructSuper:
            return resolveConstructSuper(block, index);
        case Opcode::CallBuiltin:
            return resolveCallNoInline(block, index);
        default:
            return false;
        }
    }

    // What is added, and where.

    BlockID addBlock()
    {
        m_function.blocks.append(Block { });
        m_refuted.append(Refuted { });
        m_asksClassObject.append(LayoutID());
        m_refutedShapes.append(RefutedShape { });
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

    // Takes the value at the index out of its block, which ends there and is added to from here on. What came after the value is in a
    // block of its own, and the value is the parameter of that: it keeps its number, its type and what is expected of it.
    void open(BlockID block, unsigned index)
    {
        ValueID id = m_function.at(block).values[index];
        m_position = m_function.at(id).position;
        m_inlineFrame = m_function.at(id).inlineFrame;
        m_isInTailPosition = m_function.at(id).isInTailPosition;
        bool hasDepth = m_function.at(id).opcode == Opcode::CallMethod || m_function.at(id).opcode == Opcode::Call || m_function.at(id).opcode == Opcode::Operation;
        m_inliningDepth = hasDepth ? m_function.at(id).immediates[1] : Format::none;
        m_castOfResult = std::nullopt;
        m_isWatched = m_function.at(block).terminator.kind == TerminatorKind::Invoke && index + 1 == m_function.at(block).values.size();
        m_after = addBlock();
        {
            auto& from = m_function.at(block);
            m_function.at(m_after).values.append(from.values.subspan(index + 1));
            from.values.shrink(index);
            m_function.at(m_after).terminator = std::exchange(from.terminator, Terminator { });
        }
        m_handler = { };
        if (m_isWatched) {
            auto& invoke = m_function.at(m_after).terminator;
            m_handler = invoke.targets[1];
            invoke.kind = TerminatorKind::Jump;
            invoke.targets.shrink(1);
        }
        Value parameter = make(Opcode::Parameter, m_function.at(id).type);
        parameter.takePredictionOf(m_function.at(id));
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

    // What may throw ends its block where a handler watches.
    void endBlockAfterWatchedValue()
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
        endBlockAfterWatchedValue();
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
        m_lastMayThrow = m_isWatched;
        return id;
    }

    // Goes on where the condition holds. Yields where to go on for where it does not.
    BlockID branch(ValueID condition)
    {
        endBlockAfterWatchedValue();
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

    // On to what came after the value that is replaced, which is this from here on.
    void finish(ValueID result)
    {
        auto& terminator = m_function.at(m_block).terminator;
        terminator.kind = m_lastMayThrow ? TerminatorKind::Invoke : TerminatorKind::Jump;
        terminator.targets.append(Target { m_after, Arguments { result } });
        if (m_lastMayThrow)
            terminator.targets.append(m_handler);
        m_lastMayThrow = false;
    }

    // What was appended last throws, always.
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
        endBlockAfterWatchedValue();
        auto& terminator = m_function.at(m_block).terminator;
        terminator.kind = TerminatorKind::Throw;
        terminator.value = thrown;
        if (m_isWatched)
            terminator.targets.append(m_handler);
    }

    ValueID appendGlobal(StringID name)
    {
        Value global = make(Opcode::GetGlobal, m_program.types.anything);
        global.immediates[0] = name.index();
        return appendThrowing(WTF::move(global));
    }

    ValueID appendConstructOfGlobal(StringID name, Arguments&& arguments)
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
        finishByThrowing(appendConstructOfGlobal(m_typeError, Arguments { append(WTF::move(text)) }));
    }

    ValueID addParameterTo(BlockID block, TypeID type)
    {
        Value parameter = make(Opcode::Parameter, type);
        parameter.immediates = { block.index(), static_cast<uint32_t>(m_function.at(block).parameters.size()) };
        ValueID id = addValue(WTF::move(parameter));
        m_function.at(block).parameters.append(id);
        return id;
    }

    ValueID undefinedAtHeadOf(BlockID block)
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

    void countUsesAndFindUnreachedBlocks()
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
        m_numberOfBlocksAsRead = m_function.blocks.size();
        Vector<unsigned, 16> worklist { 0u };
        m_isReached.set(0);
        while (!worklist.isEmpty()) {
            for (auto& target : m_function.blocks[worklist.takeLast()].terminator.targets) {
                if (!m_isReached.set(target.block.index()))
                    worklist.append(target.block.index());
            }
        }
        for (unsigned b = 0; b < m_numberOfBlocksAsRead; ++b) {
            if (m_isReached.get(b))
                continue;
            for (auto& target : m_function.blocks[b].terminator.targets)
                m_unreachedPredecessors.add(target.block.index(), Vector<unsigned, 1>()).iterator->value.append(b);
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
        if (definedIn.index() < m_numberOfBlocksAsRead && !m_isReached.get(definedIn.index()))
            return false;
        while (m_indexInRegion.size() < m_function.blocks.size())
            m_indexInRegion.append(UINT_MAX);
        Vector<unsigned, 32> region { definedIn.index() };
        m_indexInRegion[definedIn.index()] = 0;
        auto forgetRegion = makeScopeExit([&] {
            for (unsigned block : region)
                m_indexInRegion[block] = UINT_MAX;
        });
        Vector<Resource, maxResources> resources;
        ValueID dispose;
        BlockID disposedIn;
        unsigned numberOfUsesSeen = 0;
        for (unsigned next = 0; next < region.size(); ++next) {
            unsigned b = region[next];
            auto& block = m_function.blocks[b];
            bool isUsedOtherwise = false;
            block.terminator.forEachChild([&](ValueID child) {
                isUsedOtherwise |= child == stack;
            });
            for (unsigned i = 0; i < block.values.size() && !isUsedOtherwise; ++i) {
                auto& value = m_function.at(block.values[i]);
                unsigned uses = 0;
                value.forEachChild([&](ValueID child) {
                    uses += child == stack;
                });
                if (!uses)
                    continue;
                numberOfUsesSeen += uses;
                if (uses > 1 || value.opcode != Opcode::Operation || value.arguments.size() != 3 || value.arguments[0] != stack) {
                    isUsedOtherwise = true;
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
                    isUsedOtherwise = true;
                    continue;
                }
                resources.append(Resource { block.values[i], value.arguments[1], BlockID(b), { }, { } });
            }
            if (isUsedOtherwise)
                return false;
            if (dispose && disposedIn == BlockID(b))
                continue;
            bool isLeftWithoutStack = BlockID(b) == definedIn && block.terminator.kind == TerminatorKind::Invoke && block.values.last() == stack;
            for (unsigned t = 0; t < block.terminator.targets.size() - isLeftWithoutStack; ++t) {
                unsigned to = block.terminator.targets[t].block.index();
                if (m_indexInRegion[to] != UINT_MAX)
                    continue;
                m_indexInRegion[to] = 0;
                region.append(to);
            }
        }
        if (numberOfUsesSeen != m_numberOfUses[stack.index()] || !dispose || disposedIn == definedIn)
            return false;
        for (auto& resource : resources) {
            if (m_numberOfUses[resource.method.index()])
                return false;
        }
        std::ranges::sort(region);
        unsigned sizeOfRegion = region.size();
        for (unsigned i = 0; i < sizeOfRegion; ++i)
            m_indexInRegion[region[i]] = i;
        std::ranges::sort(resources, { }, [](const Resource& resource) {
            return resource.addedIn.index();
        });
        unsigned disposal = m_indexInRegion[disposedIn.index()];

        auto atExitOf = [&](const Resource& resource, unsigned block, unsigned target) -> uint8_t {
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
            resource.atEntryOf.fill(0, sizeOfRegion);
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
                    uint8_t joined = atEntry | atExitOf(resource, block, t);
                    if (std::exchange(atEntry, joined) != joined)
                        worklist.append(to);
                }
            }
            if (!resource.atEntryOf[disposal])
                return false;
            if (resource.addedIn != definedIn && resource.atEntryOf[m_indexInRegion[resource.addedIn.index()]] != isNotAdded)
                return false;
        }
        Vector<unsigned, maxResources> inOrderOfAdding;
        inOrderOfAdding.fill(UINT_MAX, resources.size());
        for (unsigned later = 0; later < resources.size(); ++later) {
            unsigned numberAddedBefore = 0;
            for (unsigned earlier = 0; earlier < resources.size(); ++earlier) {
                if (earlier == later)
                    continue;
                uint8_t state = resources[later].addedIn == definedIn ? isNotAdded : resources[earlier].atEntryOf[m_indexInRegion[resources[later].addedIn.index()]];
                if (state == mayBeAdded)
                    return false;
                numberAddedBefore += state == isAdded;
            }
            if (inOrderOfAdding[numberAddedBefore] != UINT_MAX)
                return false;
            inOrderOfAdding[numberAddedBefore] = later;
        }

        BitVector reachesDisposal(sizeOfRegion);
        {
            Vector<Vector<unsigned, 2>> predecessors(sizeOfRegion);
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
            resource.parametersOf.fill(ValueID(), 2 * sizeOfRegion);
            for (unsigned i = 0; i < sizeOfRegion; ++i) {
                unsigned b = region[i];
                if (BlockID(b) == definedIn || resource.atEntryOf[i] != mayBeAdded || !reachesDisposal.get(i))
                    continue;
                resource.parametersOf[2 * i] = addParameterTo(BlockID(b), anything);
                resource.parametersOf[2 * i + 1] = addParameterTo(BlockID(b), anything);
                if (auto unreached = m_unreachedPredecessors.find(b); unreached != m_unreachedPredecessors.end())
                    sources.appendVector(unreached->value);
            }
        }
        if (sources.size() > sizeOfRegion) {
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
                    uint8_t state = atExitOf(resource, b, t);
                    RELEASE_ASSERT(state != mayBeAdded || resource.parametersOf[2 * from]);
                    if (state != isAdded && state != mayBeAdded && !nothing)
                        nothing = undefinedAtHeadOf(BlockID(b));
                    targets[t].arguments.append(state == isAdded ? resource.value : state == mayBeAdded ? resource.parametersOf[2 * from] : nothing);
                    targets[t].arguments.append(state == isAdded ? resource.method : state == mayBeAdded ? resource.parametersOf[2 * from + 1] : nothing);
                }
            }
        }

        for (auto& resource : resources) {
            open(resource.addedIn, m_function.at(resource.addedIn).values.size() - 1);
            m_function.at(resource.method).type = anything;
            m_function.at(resource.method).rep = repOf(m_program.at(anything));
            BlockID isNoObject = branch(appendTest(Operations::IsObject, resource.value));
            Value symbol = make(Opcode::GetById, anything);
            symbol.children[0] = appendGlobal(m_symbol);
            symbol.immediates[0] = m_dispose.index();
            Value read = make(Opcode::GetByVal, anything);
            read.children = { resource.value, appendThrowing(WTF::move(symbol)) };
            ValueID method = appendThrowing(WTF::move(read));
            BlockID isNotCallable = branch(appendTest(Operations::IsCallable, method));
            finish(method);
            moveTo(isNotCallable);
            finishByThrowingTypeError(m_usingTakesDisposables);
            moveTo(isNoObject);
            Value isNil = make(Opcode::IsUndefinedOrNull, m_program.types.boolean);
            isNil.children[0] = resource.value;
            BlockID isNeither = branch(append(WTF::move(isNil)));
            finish(append(make(Opcode::Undefined, m_program.types.undefined)));
            moveTo(isNeither);
            finishByThrowingTypeError(m_usingTakesObjects);
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
            auto& resource = resources[inOrderOfAdding[i]];
            uint8_t state = resource.atEntryOf[disposal];
            if (state == isNotAdded)
                continue;
            ValueID value = state == isAdded ? resource.value : resource.parametersOf[2 * disposal];
            ValueID method = state == isAdded ? resource.method : resource.parametersOf[2 * disposal + 1];
            BlockID next = addBlock();
            ValueID hasErrorBehind = addParameterTo(next, m_program.types.boolean);
            ValueID errorBehind = addParameterTo(next, anything);
            ValueID hasNewErrorBehind = addParameterTo(next, m_program.types.boolean);
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
            ValueID suppressing = appendConstructOfGlobal(m_suppressedError, Arguments { thrown, error });
            jumpTo(next, Arguments { yes, suppressing, yes });
            moveTo(isFirst);
            jumpTo(next, Arguments { yes, thrown, yes });
            moveTo(next);
            hasError = hasErrorBehind;
            error = errorBehind;
            hasNewError = hasNewErrorBehind;
        }
        BlockID hasNoNewError = branch(hasNewError);
        finishByThrowing(error);
        moveTo(hasNoNewError);
        finish(append(make(Opcode::Undefined, m_program.types.undefined)));
        m_function.at(stack) = make(Opcode::Undefined, m_program.types.undefined);
        return true;
    }

    // `new F() as T`, after open(): the cast is what comes after the value, and nothing else uses the value. From here on the cast is what
    // is replaced, and each way carries it out on what it yields: see finishWithCast().
    void takeCastOfResult(ValueID id)
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
        m_castOfResult = value;
        Value parameter = make(Opcode::Parameter, value.type);
        parameter.takePredictionOf(value);
        parameter.immediates = { m_after.index(), 0 };
        m_function.at(cast) = WTF::move(parameter);
        after.values.removeAt(0);
        after.parameters[0] = cast;
    }

    void finishWithCast(ValueID result)
    {
        if (m_castOfResult) {
            Value cast = *m_castOfResult;
            cast.children[0] = result;
            result = appendThrowing(WTF::move(cast));
        }
        finish(result);
    }

    // The value as one of the type. None: anything will do.
    ValueID held(ValueID value, TypeID type, CastReason reason)
    {
        if (!type || m_program.isHeldAlready(typeOf(value), m_program.at(type), reason))
            return value;
        Value cast = make(Opcode::Cast, type);
        cast.children[0] = value;
        cast.immediates[1] = static_cast<uint32_t>(reason);
        if (reason == CastReason::As && m_literalsNobodyElseHolds.contains(value))
            cast.immediates[1] |= static_cast<uint32_t>(Format::Freshness::TopLevel) << Format::freshnessShift;
        return appendThrowing(WTF::move(cast));
    }

    ValueID heldAsParameter(FunctionID callee, ValueID value, TypeID type, CastReason reason)
    {
        ValueID result = held(value, type, reason);
        if (result != value && !m_program.at(callee).has(Format::FunctionIsBuiltin | Format::FunctionIsOfBuiltinModule | Format::FunctionIsHiddenInStackTraces))
            m_function.at(result).setParameterCheckCallee(callee);
        return result;
    }

    // Calls of a function that is known.

    bool definesFieldOf(const Value& user, ValueID literal) const
    {
        if ((user.opcode != Opcode::Operation && user.opcode != Opcode::CallBuiltin) || user.arguments.isEmpty() || user.arguments[0] != literal)
            return false;
        return m_program.operationOf(user) == Operations::DefineField;
    }

    Vector<ValueID, 2> literalsHandedToRecordParameters(ValueID call, FunctionID callee) const
    {
        Vector<ValueID, 2> result;
        auto& arguments = m_function.at(call).arguments;
        auto& parameters = m_program.at(callee).parameterTypes;
        for (unsigned k = 0; k < std::min<size_t>(arguments.size(), parameters.size()); ++k) {
            ValueID literal = arguments[k];
            if (m_function.at(literal).opcode != Opcode::NewObject || !parameters[k])
                continue;
            bool becomesRecord = m_program.at(parameters[k]).views.containsIf([&](LayoutID view) {
                return m_program.at(view).kind == LayoutKind::Record && !m_program.at(view).admitsUncheckedObjects();
            });
            if (!becomesRecord)
                continue;
            bool isHeldElsewhere = false;
            for (auto& block : m_function.blocks) {
                for (ValueID id : block.values) {
                    auto& user = m_function.at(id);
                    unsigned times = 0;
                    user.forEachChild([&](ValueID child) {
                        times += child == literal;
                    });
                    isHeldElsewhere |= times > 1 || (times && id != call && !definesFieldOf(user, literal));
                }
                block.terminator.forEachChild([&](ValueID child) {
                    isHeldElsewhere |= child == literal;
                });
            }
            if (!isHeldElsewhere)
                result.append(literal);
        }
        return result;
    }

    bool mayBeCalledDirectly(FunctionID id) const
    {
        auto& callee = m_program.at(id);
        return !callee.takesItsClosure && !callee.rampForClosures && mayBeCalledDirectlyWithClosure(id);
    }

    bool mayBeCalledDirectlyWithClosure(FunctionID id) const
    {
        auto& callee = m_program.at(id);
        if (callee.blocks.isEmpty() || callee.isGenericEntrypoint())
            return false;
        // [[Call]] of a class throws.
        if (callee.kind == FunctionKind::Constructor || callee.kind == FunctionKind::Module)
            return false;
        return mayBeHandedRest(callee);
    }

    // What is past the parameters is handed over as an array: passedTo().
    bool mayBeHandedRest(const Function& callee) const
    {
        if (!callee.has(Format::FunctionHasRestParameter))
            return true;
        if (callee.parameterTypes.isEmpty())
            return false;
        TypeID rest = callee.parameterTypes.last();
        return !rest || Type::engineClass(Builtins::singleton().arrayClass()).isWithin(m_program.at(rest), m_program);
    }

    // The arguments as the entry block of the function takes them. Whoever does not know the function goes through its generic
    // entrypoint, which holds what it is handed to the types of the parameters. That the function was found changes nothing about that:
    // the checker lets a method be called with less than it asks for.
    Arguments passedTo(FunctionID id, std::span<const ValueID> captures, ValueID thisValue, std::span<const ValueID> given)
    {
        auto& callee = m_program.at(id);
        Arguments all;
        RELEASE_ASSERT(captures.size() == callee.captureTypes.size());
        all.append(captures);
        if (callee.has(Format::FunctionHasThis))
            all.append(heldAsParameter(id, thisValue, callee.thisType, CastReason::Flow));
        unsigned count = callee.parameterTypes.size();
        for (unsigned k = 0; k < count; ++k) {
            TypeID type = callee.parameterTypes[k];
            if (callee.has(Format::FunctionHasRestParameter) && k + 1 == count) {
                Value rest = make(Opcode::NewArray, type ? type : m_program.types.anything);
                if (k < given.size())
                    rest.arguments.append(given.subspan(k));
                all.append(appendThrowing(WTF::move(rest)));
            } else if (k < given.size())
                all.append(heldAsParameter(id, given[k], type, callee.reasonToHoldParameter(k)));
            else
                all.append(heldAsParameter(id, append(make(Opcode::Undefined, m_program.types.undefined)), type, callee.reasonToHoldParameter(k)));
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

    bool holdsSomething(FunctionID id, ValueID thisValue, std::span<const ValueID> given) const
    {
        auto& callee = m_program.at(id);
        auto isHeld = [&](const Type& have, TypeID type, CastReason reason) {
            return !type || m_program.isHeldAlready(have, m_program.at(type), reason);
        };
        if (callee.has(Format::FunctionHasThis) && !isHeld(typeOf(thisValue), callee.thisType, CastReason::Flow))
            return true;
        for (unsigned k = 0; k < callee.parameterTypes.size(); ++k) {
            if (!isHeld(k < given.size() ? typeOf(given[k]) : m_program.at(m_program.types.undefined), callee.parameterTypes[k], callee.reasonToHoldParameter(k)))
                return true;
        }
        return false;
    }

    bool takesWhatMayBeForeign(FunctionID id) const
    {
        for (TypeID type : m_program.at(id).parameterTypes) {
            if (!type)
                continue;
            for (LayoutID view : m_program.at(type).views) {
                if (m_program.at(view).admitsForeignObjects)
                    return true;
            }
        }
        return false;
    }

    // `type`: of the value that is replaced.
    void callAndFinish(FunctionID id, std::span<const ValueID> captures, ValueID thisValue, std::span<const ValueID> given, TypeID type, Yields yields)
    {
        if (FunctionID written = m_program.at(id).writtenEntry; written && (holdsSomething(id, thisValue, given) || (m_program.at(written).has(FunctionLogsMisfits) && takesWhatMayBeForeign(id))))
            id = written;
        // By the language an async function throws nothing to its caller: what it refuses of what it is handed rejects. Who does not know the function goes through its general entry: there the unwinder sees to it.
        TypeID promiseType = typeOfPromiseOf(m_program, m_program.at(id));
        bool wasWatched = m_isWatched;
        Target handler = m_handler;
        BlockID refused;
        BlockID before;
        if (promiseType) {
            endBlockAfterWatchedValue();
            before = m_block;
            refused = addBlock();
            m_isWatched = true;
            m_handler = Target { refused, { } };
        }
        Arguments arguments = passedTo(id, captures, thisValue, given);
        bool mayRefuse = promiseType && (m_lastMayThrow || m_block != before);
        if (promiseType) {
            endBlockAfterWatchedValue();
            m_isWatched = wasWatched;
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

    // By the shape of the receiver.

    // What a class has under a computed name that may be any string is not known here, and comes before what the classes it extends have.
    bool isFoundFirst(LayoutID member, FunctionID callee) const
    {
        for (LayoutID at = member; at; at = m_program.at(at).base) {
            auto& layout = m_program.at(at);
            if (layout.hasMethodOfUnknownName)
                return false;
            for (auto& method : layout.methods) {
                if (method.function == callee)
                    return true;
            }
        }
        return false;
    }

    // What each member of the view has by the name. Nothing: it is not for here to tell, or there are too many.
    std::optional<Cases> casesOf(LayoutID view, StringID name, MethodKind kind, bool isTestedForMembership = false) const
    {
        // The receiver may be any object. The lowering tells those of the members from the rest, and asks the rest.
        if (m_program.at(view).admitsForeignObjects && !isTestedForMembership)
            return std::nullopt;
        Cases cases;
        for (LayoutID member : m_program.at(view).members) {
            auto& layout = m_program.at(member);
            FunctionID callee;
            if (hasMethodField(m_program, member, name))
                return std::nullopt;
            // A property of its own comes before what its class has.
            if (layout.kind == LayoutKind::Instance && !layout.fieldNamed(name)) {
                callee = findMethod(m_program, member, name, kind);
                // What a method captures is in its closure, which is on the prototype.
                if (!callee || layout.mayHaveOtherPrototype || !mayBeCalledDirectly(callee) || !m_program.at(callee).captureTypes.isEmpty() || !isFoundFirst(member, callee))
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
            cases[at].members.append(member);
        }
        if (cases.isEmpty())
            return std::nullopt;
        return cases;
    }

    // Whether, among the layouts that are left, the members of the view are those of the case.
    bool decides(LayoutID view, const Case& which, const Vector<LayoutID>& left) const
    {
        auto& members = m_program.at(view).members;
        for (LayoutID member : left) {
            if (std::ranges::binary_search(members, member) != std::ranges::binary_search(which.members, member))
                return false;
        }
        return true;
    }

    bool findTest(Case& which, const Vector<LayoutID>& left) const
    {
        which.isExact = false;
        // `this` is held to this very type if the test passes, which then costs nothing.
        if (TypeID thisType = which.callee ? m_program.at(which.callee).thisType : TypeID()) {
            which.test = m_program.at(thisType).onlyView();
            if (which.test && decides(which.test, which, left))
                return true;
        }
        // A class comes before those that extend it.
        which.test = which.members[0];
        if (decides(which.test, which, left))
            return true;
        which.isExact = true;
        return which.members.size() == 1;
    }

    // Puts the cases in an order in which one test tells each from those after it.
    bool orderCases(Cases& cases, LayoutID view) const
    {
        // No type says of an object that it has the field.
        for (unsigned i = 0; i + 1 < cases.size(); ++i) {
            if (!cases[i].callee)
                std::rotate(cases.begin() + i, cases.begin() + i + 1, cases.end());
        }
        // So that case stays the last, which is not tested for.
        unsigned numberOfCallees = cases.size() - (cases.last().callee ? 0 : 1);
        Vector<LayoutID> left = m_program.at(view).members;
        for (unsigned done = 0; done + 1 < cases.size(); ++done) {
            unsigned found = done;
            while (found < numberOfCallees && !findTest(cases[found], left))
                ++found;
            if (found == numberOfCallees)
                return false;
            std::rotate(cases.begin() + done, cases.begin() + found, cases.begin() + found + 1);
            left.removeAllMatching([&](LayoutID member) {
                return std::ranges::binary_search(cases[done].members, member);
            });
        }
        return true;
    }

    LayoutID viewExpectedOf(BlockID block, ValueID object) const
    {
        if (!m_mayGuess || !typeOf(object).anyObject)
            return { };
        TypeID prediction = m_function.at(object).prediction() ? m_function.at(object).prediction() : m_function.at(originOf(object)).prediction();
        if (!prediction)
            return { };
        LayoutID view = m_program.at(prediction).onlyView();
        if (!view || m_refutedShapes[block.index()] == RefutedShape { originOf(object), view })
            return { };
        return view;
    }

    BlockID testShape(ValueID object, LayoutID view)
    {
        Value test = make(Opcode::HasShapeOf, m_program.types.boolean);
        test.children[0] = object;
        test.immediates = { view.index(), m_program.mayRunInSeveralInstances ? shapeTestIsForThisInstance : 0u };
        BlockID otherwise = branch(append(WTF::move(test)));
        m_refutedShapes[otherwise.index()] = RefutedShape { originOf(object), view };
        return otherwise;
    }

    bool resolveCallOfExpectedMethod(BlockID block, unsigned index)
    {
        if (!m_mayGuess)
            return false;
        ValueID id = m_function.at(block).values[index];
        ValueID read = m_function.at(id).child(0);
        if (m_function.at(read).opcode != Opcode::GetById)
            return false;
        ValueID object = m_function.at(read).child(0);
        if (typeOf(object).onlyView() || originOf(object) != originOf(m_function.at(id).child(1)))
            return false;
        LayoutID view = viewExpectedOf(block, object);
        if (!view || !isObjectLayout(m_program.at(view).kind) || !isUsedOnlyBy(read, id))
            return false;
        Value original = m_function.at(id);
        auto cases = casesOf(view, m_function.at(read).string(), MethodKind::Method);
        if (!cases || !orderCases(*cases, view) || !cases->last().callee)
            return false;
        unsigned indexOfRead = index;
        while (indexOfRead && m_function.at(block).values[indexOfRead - 1] != read) {
            Opcode between = m_function.at(m_function.at(block).values[--indexOfRead]).opcode;
            if (between != Opcode::Undefined && between != Opcode::Null && between != Opcode::Boolean && between != Opcode::Integer && between != Opcode::Double && between != Opcode::String)
                return false;
        }
        if (!indexOfRead)
            return false;
        m_function.at(block).values.removeAt(indexOfRead - 1);
        open(block, index - 1);
        BlockID otherwise = testShape(object, view);
        Value cast = make(Opcode::Cast, m_program.at(view).type);
        cast.children[0] = object;
        cast.immediates = { m_function.at(read).immediates[0], static_cast<uint32_t>(CastReason::Flow) };
        ValueID receiver = appendThrowing(WTF::move(cast));
        dispatch(*cases, view, receiver, original.arguments.span(), original, Yields::Result);
        moveTo(otherwise);
        m_function.at(read).setPredictedClosure({ });
        place(read);
        m_lastMayThrow = m_isWatched;
        finish(appendThrowing(WTF::move(original)));
        return true;
    }

    bool resolveByShape(BlockID block, unsigned index, MethodKind kind)
    {
        Value original = m_function.at(m_function.at(block).values[index]);
        ValueID object = original.child(0);
        StringID name = original.string();
        LayoutID view = typeOf(object).onlyView();
        bool isExpected = !view;
        if (isExpected)
            view = viewExpectedOf(block, object);
        if (!view || !isObjectLayout(m_program.at(view).kind))
            return false;
        bool needsMembershipTest = !isExpected && kind != MethodKind::Method && m_program.at(view).admitsForeignObjects;
        auto cases = casesOf(view, name, kind, needsMembershipTest);
        if (!cases || !orderCases(*cases, view) || (isExpected && !cases->last().callee))
            return false;
        bool callsFunction = std::ranges::any_of(*cases, [](auto& which) { return !!which.callee; });
        bool needsInstanceTest = !isExpected && m_program.mayRunInSeveralInstances && callsFunction;
        if ((needsInstanceTest || needsMembershipTest) && m_refutedShapes[block.index()] == RefutedShape { originOf(object), view })
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
            otherwise = testShape(object, view);
        // The read of a field sees to it itself.
        if ((*cases)[0].callee && (isExpected || typeOf(object).mayBeUndefinedOrNull())) {
            Value cast = make(Opcode::Cast, m_program.at(view).type);
            cast.children[0] = object;
            cast.immediates = { name.index(), static_cast<uint32_t>(CastReason::Flow) };
            receiver = appendThrowing(WTF::move(cast));
        }
        dispatch(*cases, view, receiver, given.span(), original, kind == MethodKind::Setter ? Yields::Undefined : Yields::Result);
        if (otherwise) {
            moveTo(otherwise);
            ValueID result = appendThrowing(Value(original));
            if (kind == MethodKind::Setter)
                result = held(append(make(Opcode::Undefined, m_program.types.undefined)), original.type, CastReason::Claim);
            finish(result);
        }
        return true;
    }

    // The receiver is an object.
    void dispatch(const Cases& cases, LayoutID view, ValueID receiver, std::span<const ValueID> given, const Value& original, Yields yields)
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
                callAndFinish(which.callee, { }, receiver, given, original.type, yields);
            else
                callValueOfFieldAndFinish(view, receiver, original);
            if (otherwise)
                moveTo(otherwise);
        }
    }

    // `o.m(f())` reads `m` before it calls `f`: the front end writes a read, and a call of what was read with `o` for `this`. A method of
    // a class cannot be replaced, so it makes no difference when it is looked for. What is left of the read is that it throws where
    // there is no object.
    bool resolveCallOfMethodRead(BlockID block, unsigned index)
    {
        ValueID id = m_function.at(block).values[index];
        Value original = m_function.at(id);
        ValueID read = original.child(0);
        if (m_function.at(read).opcode != Opcode::GetById)
            return false;
        ValueID object = m_function.at(read).child(0);
        StringID name = m_function.at(read).string();
        LayoutID view = typeOf(object).onlyView();
        if (originOf(object) != originOf(original.child(1)) || !view || !isObjectLayout(m_program.at(view).kind) || m_program.mayRunInSeveralInstances)
            return false;
        auto cases = casesOf(view, name, MethodKind::Method);
        // What a field holds by the time of the call is another matter.
        if (!cases || !orderCases(*cases, view) || !cases->last().callee || !isUsedOnlyBy(read, id))
            return false;

        bool mayBeNil = typeOf(object).mayBeUndefinedOrNull();
        auto& receiver = m_function.at(read);
        receiver.opcode = Opcode::Cast;
        receiver.type = mayBeNil ? m_program.at(view).type : m_function.at(object).type;
        receiver.rep = repOf(m_program.at(receiver.type));
        receiver.immediates = { mayBeNil ? name.index() : Format::none, static_cast<uint32_t>(mayBeNil ? CastReason::Flow : CastReason::Upcast) };
        if (!mayBeNil) {
            for (auto& other : m_function.blocks) {
                if (other.terminator.kind != TerminatorKind::Invoke || other.values.isEmpty() || other.values.last() != read)
                    continue;
                other.terminator.kind = TerminatorKind::Jump;
                other.terminator.targets.shrink(1);
            }
        }
        open(block, index);
        dispatch(*cases, view, read, original.arguments.span(), original, Yields::Result);
        return true;
    }

    // `o.f(x)` where `f` is a field: the field is read, and what it holds is called.
    void callValueOfFieldAndFinish(LayoutID view, ValueID receiver, const Value& original)
    {
        TypeID type;
        if (auto field = m_program.at(view).fieldNamed(original.string()))
            type = m_program.at(view).fields[*field].type;
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

    // `o.f(x)` through a view that admits foreign objects, which resolveByShape() leaves to the lowering. Where something is expected of what
    // the field holds, the read becomes a value of its own, so that the call can go by that. The arguments are evaluated already.
    bool resolveCallOfField(BlockID block, unsigned index)
    {
        Value original = m_function.at(m_function.at(block).values[index]);
        auto access = fieldOf(m_program, typeOf(original.child(0)), original.string());
        if (!access || !m_program.at(access->layout).fields[access->field].onlyFunction)
            return false;
        open(block, index);
        callValueOfFieldAndFinish(access->layout, original.child(0), original);
        return true;
    }

    // On a class object, and behind a class.

    // What a class declares under one name.
    struct Member {
        FunctionID method;
        FunctionID getter;
        FunctionID setter;
    };

    // The class that is evaluated once whose class object the value is, going by how it is defined.
    LayoutID classDefinedBy(ValueID value) const
    {
        auto& definition = m_function.at(originOf(value));
        return definition.opcode == Opcode::FunctionReference ? m_program.at(definition.function()).constructedClass : LayoutID();
    }

    // What a search by the name comes to that starts at the class object of the class, or at its prototype object. These objects keep their
    // prototypes and what the classes declare, but for what the program is seen to replace, and get nothing by the name of what they
    // inherit. Nothing: it is not for here to tell, or the search leaves the classes of the program.
    std::optional<Member> memberFoundFrom(LayoutID start, StringID name, bool isStatic) const
    {
        for (LayoutID at = start; at; at = m_program.at(at).base) {
            auto& layout = m_program.at(at);
            if (isStatic ? !layout.hasFixedMembers || layout.hasStaticMemberOfUnknownName : !layout.isLaidOut || layout.hasMethodOfUnknownName)
                return std::nullopt;
            // A static field that is reached through a class that extends its class is read and stored to as the language says.
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
            // Every function has these of its own, whatever it inherits.
            auto text = m_program.string(name);
            if (isStatic && (equalSpans(text, asBytes("length"_s.span8())) || equalSpans(text, asBytes("name"_s.span8())) || equalSpans(text, asBytes("prototype"_s.span8()))))
                return std::nullopt;
        }
        return std::nullopt;
    }

    // A class that is evaluated once closes over nothing, and neither does what it declares.
    bool mayBeCalledWithoutEnvironment(FunctionID id) const { return mayBeCalledDirectly(id) && m_program.at(id).captureTypes.isEmpty(); }

    // After open(), for an access to a static member: goes on where the class object of the class is as the classes declare it. In the other
    // arm the operation is carried out as it stands, on the class object, by name.
    void askClassObjectUnlessAsDeclared(LayoutID ofClass, const Value& original)
    {
        if (!ofClass)
            return;
        Value test = make(Opcode::IsClassObjectAsDeclared, m_program.types.boolean);
        test.immediates[0] = ofClass.index();
        BlockID asks = branch(append(WTF::move(test)));
        BlockID isAsDeclared = m_block;
        m_asksClassObject[asks.index()] = ofClass;
        moveTo(asks);
        ValueID result = appendThrowing(Value(original));
        if (original.opcode == Opcode::PutById)
            result = held(append(make(Opcode::Undefined, m_program.types.undefined)), original.type, CastReason::Claim);
        finish(result);
        moveTo(isAsDeclared);
    }

    // The value at the index reads the member, with that for `this`. `guarded`: the class whose class object is the receiver, for a static member.
    bool readMember(BlockID block, unsigned index, const Member& member, ValueID thisValue, TypeID type, LayoutID guarded = { })
    {
        Value original = m_function.at(m_function.at(block).values[index]);
        if (member.getter) {
            if (!mayBeCalledWithoutEnvironment(member.getter))
                return false;
            open(block, index);
            askClassObjectUnlessAsDeclared(guarded, original);
            callAndFinish(member.getter, { }, thisValue, { }, type, Yields::Result);
            return true;
        }
        if (member.method && (!m_program.at(member.method).captureTypes.isEmpty() || !m_program.genericEntrypoints.contains(member.method.index())))
            return false;
        open(block, index);
        askClassObjectUnlessAsDeclared(guarded, original);
        // An accessor without a getter reads `undefined`.
        Value read = member.method ? make(Opcode::FunctionReference, m_program.types.function) : make(Opcode::Undefined, m_program.types.undefined);
        if (member.method)
            read.immediates[0] = member.method.index();
        finish(held(append(WTF::move(read)), type, CastReason::Claim));
        return true;
    }

    // Only a setter takes a store. The language has an error for any other, which is left to the run time.
    bool storeToMember(BlockID block, unsigned index, const Member& member, ValueID thisValue, ValueID stored, TypeID type, LayoutID guarded = { })
    {
        if (!member.setter || !mayBeCalledWithoutEnvironment(member.setter))
            return false;
        Value original = m_function.at(m_function.at(block).values[index]);
        open(block, index);
        askClassObjectUnlessAsDeclared(guarded, original);
        callAndFinish(member.setter, { }, thisValue, std::span { &stored, 1 }, type, Yields::Undefined);
        return true;
    }

    bool resolveCallOfStaticMethodRead(BlockID block, unsigned index, FunctionID method, ValueID classObject, LayoutID ofClass)
    {
        if (!mayBeCalledWithoutEnvironment(method))
            return false;
        auto& values = m_function.at(block).values;
        ValueID read = values[index];
        unsigned indexOfCall = index + 1;
        for (; indexOfCall < values.size(); ++indexOfCall) {
            Opcode between = m_function.at(values[indexOfCall]).opcode;
            if (between != Opcode::Undefined && between != Opcode::Null && between != Opcode::Boolean && between != Opcode::Integer && between != Opcode::Double && between != Opcode::String)
                break;
        }
        if (indexOfCall == values.size())
            return false;
        ValueID id = values[indexOfCall];
        Value original = m_function.at(id);
        if (original.opcode != Opcode::Call || original.child(0) != read || originOf(original.child(1)) != originOf(classObject) || !isUsedOnlyBy(read, id))
            return false;
        values.removeAt(index);
        open(block, indexOfCall - 1);
        Value test = make(Opcode::IsClassObjectAsDeclared, m_program.types.boolean);
        test.immediates[0] = ofClass.index();
        BlockID asks = branch(append(WTF::move(test)));
        BlockID isAsDeclared = m_block;
        m_asksClassObject[asks.index()] = ofClass;
        moveTo(asks);
        Value call = original;
        if (m_function.at(classObject).opcode == Opcode::FunctionReference && original.child(1) == classObject) {
            ValueID again = append(Value(m_function.at(classObject)));
            m_function.at(read).children[0] = again;
            call.children[1] = again;
        }
        place(read);
        m_lastMayThrow = m_isWatched;
        finish(appendThrowing(WTF::move(call)));
        moveTo(isAsDeclared);
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
        if (isStore && !m_program.isHeldAlready(typeOf(access.child(1)), m_program.at(fields[field].type), CastReason::Claim))
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

    // `C.name(..)`, `C.name`, `C.name = value`
    bool resolveOnClassObject(BlockID block, unsigned index)
    {
        Value original = m_function.at(m_function.at(block).values[index]);
        ValueID classObject = original.child(0);
        LayoutID ofClass = classDefinedBy(classObject);
        if (!ofClass || m_asksClassObject[block.index()] == ofClass)
            return false;
        if ((original.opcode == Opcode::GetById || original.opcode == Opcode::PutById) && accessStaticField(block, index, ofClass))
            return false;
        auto member = memberFoundFrom(ofClass, original.string(), true);
        if (!member)
            return false;
        switch (original.opcode) {
        case Opcode::GetById:
            if (member->method && resolveCallOfStaticMethodRead(block, index, member->method, classObject, ofClass))
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
        askClassObjectUnlessAsDeclared(ofClass, original);
        callAndFinish(member->method, { }, classObject, original.arguments.span(), original.type, Yields::Result);
        return true;
    }

    // SuperGet class, key, this, isStatic. SuperSet class, key, value, this, isStatic. The call of what `super.name` yields is resolved by
    // the definition of its callee.
    bool resolveSuper(BlockID block, unsigned index)
    {
        Value original = m_function.at(m_function.at(block).values[index]);
        bool isGet = m_program.operationOf(original) == Operations::SuperGet;
        if ((!isGet && m_program.operationOf(original) != Operations::SuperSet) || original.arguments.size() != (isGet ? 4u : 5u))
            return false;
        LayoutID around = classDefinedBy(original.arguments[0]);
        ValueID keyID = m_program.constantOf(m_function, original.arguments[1]);
        ValueID isStaticID = m_program.constantOf(m_function, original.arguments.last());
        if (!keyID || !isStaticID)
            return false;
        auto& key = m_function.at(keyID);
        auto& isStatic = m_function.at(isStaticID);
        if (!around || key.opcode != Opcode::String || isStatic.opcode != Opcode::Boolean || (isStatic.boolean() && m_asksClassObject[block.index()] == m_program.at(around).base))
            return false;
        // What the search starts behind keeps its prototype.
        if (!m_program.at(around).isLaidOut || !m_program.at(around).hasFixedMembers)
            return false;
        auto member = memberFoundFrom(m_program.at(around).base, key.string(), isStatic.boolean());
        if (!member)
            return false;
        LayoutID guarded = isStatic.boolean() ? m_program.at(around).base : LayoutID();
        if (isGet)
            return readMember(block, index, *member, original.arguments[2], original.type, guarded);
        return storeToMember(block, index, *member, original.arguments[3], original.arguments[2], original.type, guarded);
    }

    // By the definition of the callee.

    std::optional<KnownCallee> knownCallee(ValueID value) const
    {
        ValueID operand = value;
        FunctionID predicted;
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
            if (variable.onlyFunctionIsFact || variable.onlyFunctionOrNil)
                return KnownCallee { variable.onlyFunction, value, { }, variable.onlyFunctionIsFact, true };
            predicted = variable.onlyFunction;
            break;
        }
        case Opcode::GetField: {
            auto& field = m_program.at(definition.layout()).fields[definition.field()];
            if (field.onlyFunctionIsFact || field.onlyFunctionOrNil)
                return KnownCallee { field.onlyFunction, value, { }, field.onlyFunctionIsFact, true };
            if (field.otherFunction)
                return KnownCallee { field.onlyFunction, value, { }, false, false, field.otherFunction };
            predicted = field.onlyFunction;
            break;
        }
        case Opcode::GetById:
            if (auto access = fieldOf(m_program, typeOf(definition.child(0)), definition.string()))
                predicted = m_program.at(access->layout).fields[access->field].onlyFunction;
            else if (definition.predictedByKey())
                predicted = m_program.functionByKey(definition.predictedByKey(), definition.string());
            break;
        case Opcode::GetByVal:
            if (ValueID key = m_program.constantOf(m_function, definition.child(1)); definition.predictedByKey() && key && m_function.at(key).opcode == Opcode::String)
                predicted = m_program.functionByKey(definition.predictedByKey(), m_function.at(key).string());
            break;
        default:
            break;
        }
        // What is expected of the value itself comes from closer by than what is expected of the place.
        if (FunctionID ofValue = m_function.at(operand).predictedClosure() ? m_function.at(operand).predictedClosure() : definition.predictedClosure())
            predicted = ofValue;
        // Only a function with a generic entrypoint is ever made a value of.
        if (!predicted || !m_program.genericEntrypoints.contains(predicted.index()))
            return std::nullopt;
        return KnownCallee { predicted, value, { }, false };
    }

    // Once the operand is known to be a closure of the function.
    Arguments capturesOf(const KnownCallee& known, ValueID callee)
    {
        auto& types = m_program.at(known.function).captureTypes;
        Arguments captures;
        for (unsigned i = 0; i < types.size(); ++i) {
            // A class object is made with the environment of its class, whether its constructor takes that or not.
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

    // Where a test for a function has failed, the general call that is made there stays.
    void noteRefutedCallees()
    {
        m_refuted.fill(Refuted { }, m_function.blocks.size());
        m_asksClassObject.fill(LayoutID(), m_function.blocks.size());
        m_refutedShapes.fill(RefutedShape { }, m_function.blocks.size());
        for (auto& block : m_function.blocks) {
            if (block.terminator.kind != TerminatorKind::Branch)
                continue;
            auto& condition = m_function.at(block.terminator.value);
            if (condition.opcode == Opcode::IsClosureOf)
                m_refuted[block.terminator.targets[1].block.index()] = Refuted { originOf(condition.child(0)), condition.function() };
            if (condition.opcode == Opcode::IsClassObjectAsDeclared)
                m_asksClassObject[block.terminator.targets[1].block.index()] = condition.layout();
            if (condition.opcode == Opcode::HasShapeOf && condition.immediates[1] != shapeTestIsExact)
                m_refutedShapes[block.terminator.targets[1].block.index()] = RefutedShape { originOf(condition.child(0)), condition.layout() };
            // It is a closure of no function there.
            if (condition.opcode == Opcode::IsUndefinedOrNull)
                m_refuted[block.terminator.targets[0].block.index()] = Refuted { originOf(condition.child(0)), FunctionID() };
        }
    }

    bool isRefuted(BlockID block, const KnownCallee& known) const
    {
        if (m_refuted[block.index()] == Refuted { known.definition, known.function } || m_refuted[block.index()] == Refuted { known.definition, FunctionID() })
            return true;
        // The test for a function bound only stands behind the test for the function: see resolveCall().
        FunctionID bound = m_program.boundFunction(known.function, 0);
        return bound && m_refuted[block.index()] == Refuted { known.definition, bound };
    }

    // Goes on where the value is a closure of the function. Yields where to go on for where it is not.
    BlockID testClosure(ValueID callee, const KnownCallee& known)
    {
        if (known.isCertainUnlessNil) {
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
        if (known.other || m_program.at(known.function).closuresAreGivenProperties)
            test.immediates[1] = closureEvenIfAltered;
        BlockID otherwise = branch(append(WTF::move(test)));
        m_refuted[otherwise.index()] = Refuted { known.definition, known.function };
        return otherwise;
    }

    bool resolveCall(BlockID block, unsigned index)
    {
        Value original = m_function.at(m_function.at(block).values[index]);
        auto known = knownCallee(original.child(0));
        // A property of its own by the name of a method of its class: `this.m = this.m.bind(this)`.
        if (!known) {
            if (FunctionID method = methodExpectedOf(originOf(original.child(0))); method && m_program.boundFunction(method, 0))
                known = KnownCallee { method, originOf(original.child(0)), { }, false };
        }
        if (!known || !mayBeCalledDirectlyWithClosure(known->function) || isRefuted(block, *known))
            return false;
        if (known->other && !mayBeCalledDirectlyWithClosure(known->other))
            return false;
        ValueID id = m_function.at(block).values[index];
        Vector<ValueID, 2> literals;
        if (!known->isCertain)
            literals = literalsHandedToRecordParameters(id, known->function);
        if (!m_mayGuess && !known->isCertain && !known->isCertainUnlessNil && !known->other && literals.isEmpty())
            return false;
        open(block, index);
        BlockID otherwise;
        if (!known->isCertain) {
            // `f(x) as T`: what the function yields is of a type that is known, and as a rule held already. What the general call yields is not.
            takeCastOfResult(id);
            otherwise = testClosure(original.child(0), *known);
        }
        FunctionID called = m_program.at(known->function).rampForClosures ? m_program.at(known->function).rampForClosures : known->function;
        Arguments given;
        if (m_program.at(called).takesItsClosure)
            given.append(original.child(0));
        given.append(original.arguments.span());
        m_literalsNobodyElseHolds = WTF::move(literals);
        callAndFinish(called, capturesOf(*known, original.child(0)).span(), original.child(1), given.span(), original.type, Yields::Result);
        m_literalsNobodyElseHolds.clear();
        if (otherwise && known->other) {
            moveTo(otherwise);
            KnownCallee second { known->other, known->definition, { }, false, true };
            BlockID isNil = testClosure(original.child(0), second);
            FunctionID calledOtherwise = m_program.at(second.function).rampForClosures ? m_program.at(second.function).rampForClosures : second.function;
            Arguments givenOtherwise;
            if (m_program.at(calledOtherwise).takesItsClosure)
                givenOtherwise.append(original.child(0));
            givenOtherwise.append(original.arguments.span());
            callAndFinish(calledOtherwise, capturesOf(second, original.child(0)).span(), original.child(1), givenOtherwise.span(), original.type, Yields::Result);
            moveTo(isNil);
            original.arguments.clear();
            appendThrowing(WTF::move(original));
            finishThrowing();
            return true;
        }
        if (otherwise) {
            moveTo(otherwise);
            // What is expected to be the function may be the function bound.
            if (FunctionID bound = known->isCertainUnlessNil ? FunctionID() : m_program.boundFunction(known->function, 0); bound && m_program.at(bound).kind == FunctionKind::Bound) {
                KnownCallee second { bound, known->definition, { }, false };
                BlockID neither = testClosure(original.child(0), second);
                callAndFinish(bound, capturesOf(second, original.child(0)).span(), original.child(1), original.arguments.span(), original.type, Yields::Result);
                moveTo(neither);
            }
            if (known->isCertainUnlessNil)
                original.arguments.clear();
            ValueID general = appendThrowing(WTF::move(original));
            if (known->isCertainUnlessNil)
                finishThrowing();
            else
                finishWithCast(general);
        }
        return true;
    }

    // CallNoInline callee, this, arguments..: of the prelude, in the loop for whatever is no plain array. Of a function that is known for certain
    // it is a direct call, which takes what the function captures from where the closure is made and does not need the closure.
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
        value.immediates = { Format::none, depthThatIsNotInlined };
        value.arguments = WTF::move(passed);
        return resolveCall(block, index);
    }

    static unsigned numberOfArgumentsBoundBy(const Value& bind) { return bind.arguments.isEmpty() ? 0 : bind.arguments.size() - 1; }

    // `x.m`, as a value, where every object that `x` may be by its type is of a class that has one and the same method by the name. What the object has of its own, and what the
    // program has made of the prototype, is not asked: this is what is expected.
    FunctionID methodExpectedOf(ValueID read) const
    {
        auto& definition = m_function.at(read);
        if (definition.opcode != Opcode::GetById)
            return { };
        LayoutID view = typeOf(definition.child(0)).onlyView();
        if (!view || !isObjectLayout(m_program.at(view).kind))
            return { };
        FunctionID expected;
        for (LayoutID member : m_program.at(view).members) {
            FunctionID method = m_program.at(member).kind == LayoutKind::Instance ? findMethod(m_program, member, definition.string(), MethodKind::Method) : FunctionID();
            if (!method || (expected && expected != method))
                return { };
            expected = method;
        }
        return expected;
    }

    // `f.bind(t, a)`: the function that `f` is, or is expected to be, a closure of, where a closure can stand for what `bind` yields. See createBoundFunctions().
    std::optional<KnownCallee> calleeBoundBy(const Value& bind) const
    {
        static const BuiltinName name = Builtins::singleton().nameOf(asBytes("bind"_s.span8()));
        if (bind.opcode != Opcode::CallMethod || m_program.builtinName(bind.string()) != name || numberOfArgumentsBoundBy(bind) > Program::maxBoundArguments)
            return std::nullopt;
        auto known = knownCallee(bind.child(0));
        if (!known) {
            FunctionID method = methodExpectedOf(originOf(bind.child(0)));
            if (!method || !m_program.genericEntrypoints.contains(method.index()))
                return std::nullopt;
            known = KnownCallee { method, originOf(bind.child(0)), { }, false };
        }
        auto& callee = m_program.at(known->function);
        // What constructs is left to the engine's bound function. So is a generator function.
        if ((callee.kind != FunctionKind::Method && callee.kind != FunctionKind::Arrow) || callee.declaredAs & functionIsGenerator)
            return std::nullopt;
        if (!mayBeCalledDirectly(known->function) || callee.has(Format::FunctionHasRestParameter) || callee.has(Format::FunctionIsBuiltin))
            return std::nullopt;
        unsigned numberOfBoundArguments = std::min<unsigned>(numberOfArgumentsBoundBy(bind), callee.parameterTypes.size());
        // A closure has a byte for how many it captures.
        if (callee.captureTypes.size() + 2 + numberOfBoundArguments > UINT8_MAX)
            return std::nullopt;
        // A cast would throw here. What does not fit throws where the bound function is called.
        if (callee.has(Format::FunctionHasThis) && (bind.arguments.isEmpty() ? !m_program.at(m_program.types.undefined).isWithin(m_program.at(callee.thisType ? callee.thisType : m_program.types.anything), m_program) : !isWithin(bind.arguments[0], callee.thisType)))
            return std::nullopt;
        for (unsigned i = 0; i < numberOfBoundArguments; ++i) {
            if (!isWithin(bind.arguments[i + 1], callee.parameterTypes[i]))
                return std::nullopt;
        }
        return known;
    }

    bool resolveBind(BlockID block, unsigned index)
    {
        Value original = m_function.at(m_function.at(block).values[index]);
        auto known = calleeBoundBy(original);
        if (!known || isRefuted(block, *known))
            return false;
        FunctionID bound = m_program.boundFunction(known->function, numberOfArgumentsBoundBy(original));
        if (!bound)
            return false;
        auto& callee = m_program.at(known->function);
        open(block, index);
        // Also of what is known to be a closure of the function: the program may have given it a `name`.
        Value test = make(Opcode::IsClosureOf, m_program.types.boolean);
        test.children[0] = original.child(0);
        test.immediates = { known->function.index(), closureIsAsMade };
        BlockID otherwise = branch(append(WTF::move(test)));
        m_refuted[otherwise.index()] = Refuted { known->definition, known->function };

        Value closure = make(Opcode::NewClosure, m_program.types.function);
        closure.immediates[0] = bound.index();
        closure.arguments = capturesOf(*known, original.child(0));
        if (callee.has(Format::FunctionHasThis))
            closure.arguments.append(original.arguments.isEmpty() ? append(make(Opcode::Undefined, m_program.types.undefined)) : original.arguments[0]);
        for (unsigned i = 0; i < numberOfArgumentsBoundBy(original) && i < callee.parameterTypes.size(); ++i)
            closure.arguments.append(original.arguments[i + 1]);
        if (closure.arguments.size() < m_program.at(bound).captureTypes.size())
            closure.arguments.append(original.child(0));
        finish(held(append(WTF::move(closure)), original.type, CastReason::Claim));

        moveTo(otherwise);
        finish(appendThrowing(WTF::move(original)));
        return true;
    }

    // `Array.prototype.slice`, `Math.max`
    bool isReadFromGlobal(ValueID value) const
    {
        value = originOf(value);
        while (m_function.at(value).opcode == Opcode::GetById)
            value = originOf(m_function.at(value).child(0));
        return m_function.at(value).opcode == Opcode::GetGlobal;
    }

    bool hasGuard(ValueID method) const
    {
        for (auto& value : m_function.values) {
            if (value.opcode == Opcode::IsBuiltinFunction && value.child(0) == method)
                return true;
        }
        return false;
    }

    TypeID typeYieldedBy(BuiltinID first) const
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

    struct ExpectedBuiltinCallee {
        BuiltinPath path;
        BuiltinName global;
        BuiltinID entry;
        BuiltinID called;
    };

    std::optional<ExpectedBuiltinCallee> builtinCalleeAt(const Value& site, const BuiltinPath& path) const
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
        return ExpectedBuiltinCallee { path, global, entry, called };
    }

    bool isCalledAsOnItsHolder(const Value& site, const ExpectedBuiltinCallee& callee) const
    {
        if (site.opcode != Opcode::Call || !callee.entry || callee.path.isOnPrototype || (site.child(1) && !typeOf(site.child(1)).isUndefinedOrNull()))
            return false;
        auto& builtins = Builtins::singleton();
        return builtins.classNamed(callee.global) != &builtins.promiseClass() && builtins.at(callee.entry).holderName != BuiltinName(AOT_TYPED_BUILTIN_NAME_TypedArray);
    }

    bool isMethodOfLiteralCalledOnIt(const Value& site) const
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

    bool resolveCallOfExpectedBuiltin(BlockID block, unsigned index)
    {
        ValueID id = m_function.at(block).values[index];
        Value original = m_function.at(id);
        ValueID operand = original.child(0);
        if (isReadFromGlobal(operand) || isMethodOfLiteralCalledOnIt(original))
            return false;
        ExpectedBuiltins expected = m_program.expectedBuiltins(m_function, operand);
        std::optional<StringID> prototype = m_program.find(asBytes("prototype"_s.span8()));
        Vector<ExpectedBuiltinCallee, 2> callees;
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
            joined.setPrediction(typeYieldedBy(callees[0].called));
            if (callees.size() > 1 && typeYieldedBy(callees[1].called) != joined.prediction())
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
            if (isCalledAsOnItsHolder(original, callee)) {
                direct.opcode = Opcode::CallMethod;
                direct.children = { builtin, ValueID(), ValueID() };
                direct.immediates[0] = callee.path.member.index();
            } else {
                if (callee.path.isOnPrototype)
                    builtin = appendRead(builtin, *prototype);
                if (callee.path.member)
                    builtin = appendRead(builtin, callee.path.member);
                direct.children[0] = builtin;
            }
            finish(appendThrowing(WTF::move(direct)));
            moveTo(otherwise);
        }
        finish(appendThrowing(WTF::move(original)));
        return true;
    }

    // `f.call(..)`, `f.apply(..)`: as a call by name, or as a read of the method and a call of what was read with `f` for `this`.
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
        // resolveBuiltins() knows what `Array.prototype.slice.call(..)` comes down to. What it leaves, the lowering guards.
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

    // `new` on it makes an object, calls it with that for `this`, and yields the object. The object has the `prototype` of the one
    // closure there is of the function for its prototype: see Allocation::constructedBy.
    bool mayConstructDirectly(FunctionID id, bool evenWithManyClosures = false) const
    {
        auto& callee = m_program.at(id);
        if (!mayBeCalledDirectly(id) || callee.kind != FunctionKind::Plain || !callee.captureTypes.isEmpty() || (callee.hasManyClosures && !evenWithManyClosures) || !callee.has(Format::FunctionHasThis) || callee.has(Format::FunctionHasNewTarget))
            return false;
        return !mayReturnObject(callee);
    }

    // An object that it returns would take the place of the one that was made.
    bool mayReturnObject(const Function& callee) const
    {
        for (auto& block : callee.blocks) {
            if (block.terminator.kind == TerminatorKind::Return && block.terminator.value && m_program.typeOf(callee, block.terminator.value).hasObjects())
                return true;
        }
        return false;
    }

    // `new` on the class makes an object of its layout, which comes with the prototype, and calls the constructor with that for `this`.
    bool mayConstructClassDirectly(FunctionID id) const
    {
        auto& constructor = m_program.at(id);
        if (constructor.kind != FunctionKind::Constructor || constructor.blocks.isEmpty() || constructor.isGenericEntrypoint() || !constructor.constructedClass)
            return false;
        auto& ofClass = m_program.at(constructor.constructedClass);
        if (!ofClass.isLaidOut || !ofClass.isEvaluatedOnce || !ofClass.isAllocated)
            return false;
        if (!constructor.captureTypes.isEmpty() || !constructor.has(Format::FunctionHasThis) || constructor.has(Format::FunctionHasNewTarget | Format::FunctionHasRestParameter))
            return false;
        return !mayReturnObject(constructor);
    }

    // `new` on it makes an ordinary object, as State::createThis() does, and calls it with that for `this`. A class that extends another
    // is called without: it hands back what `super(..)` has made.
    bool constructsOrdinaryObject(FunctionID id) const
    {
        auto& callee = m_program.at(id);
        if (callee.has(Format::FunctionIsAsync | Format::FunctionIsGenerator) || callee.resumeFunction || callee.startFunction || callee.thisType)
            return false;
        // It reads `new.target` from where only the run time puts it.
        if (callee.kind == FunctionKind::Plain)
            return !callee.has(Format::FunctionHasNewTarget) && mayBeCalledDirectly(id) && !m_program.mayMakeSlimClosures(callee);
        if (callee.kind != FunctionKind::Constructor || callee.blocks.isEmpty() || callee.isGenericEntrypoint() || !callee.constructedClass || !mayBeHandedRest(callee))
            return false;
        auto& ofClass = m_program.at(callee.constructedClass);
        return !ofClass.isLaidOut && !ofClass.base;
    }

    bool resolveConstructOfOrdinaryObject(BlockID block, unsigned index, const KnownCallee& known)
    {
        Value original = m_function.at(m_function.at(block).values[index]);
        auto& callee = m_program.at(known.function);
        open(block, index);
        BlockID otherwise;
        if (!known.isCertain)
            otherwise = testClosure(original.child(0), known);
        // Every `super(..)` on the way up goes by its declaration. All are asked here, ahead of the birth: what is copied in need not ask again, and is a straight line.
        BlockID asks;
        for (LayoutID ofClass : classesWhoseSuperIsResolved(known.function)) {
            Value test = make(Opcode::IsClassObjectAsDeclared, m_program.types.boolean);
            test.immediates[0] = ofClass.index();
            BlockID isNot = branch(append(WTF::move(test)));
            m_asksClassObject[isNot.index()] = ofClass;
            if (!asks) {
                asks = isNot;
                continue;
            }
            m_function.at(isNot).terminator.kind = TerminatorKind::Jump;
            m_function.at(isNot).terminator.targets.append(Target { asks, { } });
        }
        Arguments captures = capturesOf(known, original.child(0));
        bool isMadeBySuperclass = callee.kind == FunctionKind::Constructor && m_program.at(callee.constructedClass).hasForeignRoot;
        Value object = make(isMadeBySuperclass ? Opcode::Undefined : Opcode::CreateThis, isMadeBySuperclass ? m_program.types.undefined : m_program.types.object);
        if (!isMadeBySuperclass)
            object.children[0] = original.child(0);
        ValueID made = append(WTF::move(object));
        // A constructor takes `new.target` for its first argument.
        Arguments given;
        if (callee.has(Format::FunctionHasNewTarget))
            given.append(original.child(0));
        given.appendVector(original.arguments);
        ValueID returned = call(known.function, passedTo(known.function, captures.span(), made, given.span()));
        m_function.at(returned).constructs = true;
        if (isMadeBySuperclass)
            made = returned;
        else if (mayReturnObject(callee)) {
            Value test = make(Opcode::CallBuiltin, m_program.types.boolean);
            test.immediates[0] = Operations::IsObject.index();
            test.arguments.append(returned);
            BlockID isNoObject = branch(append(WTF::move(test)));
            finish(held(returned, original.type, CastReason::Claim));
            moveTo(isNoObject);
        }
        finish(held(made, original.type, CastReason::Claim));
        if (asks) {
            moveTo(asks);
            finish(appendThrowing(Value(original)));
        }
        if (otherwise) {
            moveTo(otherwise);
            if (known.isCertainUnlessNil)
                original.arguments.clear();
            ValueID general = appendThrowing(WTF::move(original));
            if (known.isCertainUnlessNil)
                finishThrowing();
            else
                finish(general);
        }
        return true;
    }

    bool superIsResolved(LayoutID ofClass) const
    {
        FunctionID parent = m_program.at(ofClass).declaredParentConstructor;
        return parent && constructsOrdinaryObject(parent) && m_program.at(parent).captureTypes.isEmpty();
    }

    // The class that the function constructs and those it extends, as far up as resolveConstructSuper() finds the constructor.
    Vector<LayoutID, 4> classesWhoseSuperIsResolved(FunctionID constructor) const
    {
        static constexpr unsigned maxLevels = 8;
        Vector<LayoutID, 4> result;
        if (m_program.at(constructor).kind != FunctionKind::Constructor)
            return result;
        for (LayoutID ofClass = m_program.at(constructor).constructedClass; ofClass && superIsResolved(ofClass) && result.size() < maxLevels; ofClass = m_program.at(m_program.at(ofClass).declaredParentConstructor).constructedClass)
            result.append(ofClass);
        return result;
    }

    // The class object has passed the test on the one way that leads to the value, and nothing has come since that may run code.
    bool isKnownAsDeclared(BlockID block, unsigned index, LayoutID ofClass) const
    {
        for (unsigned steps = 0; steps < 64; ++steps) {
            for (unsigned i = index; i--;) {
                switch (m_function.at(m_function.at(block).values[i]).opcode) {
                // What these change is summed up for whole functions, later: writesOf() has no answer.
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
                if (condition.opcode == Opcode::IsClassObjectAsDeclared && condition.layout() == ofClass)
                    return terminator.targets[0].block == block;
            } else if (terminator.kind != TerminatorKind::Jump)
                return false;
            block = predecessor;
            index = m_function.at(block).values.size();
        }
        return false;
    }

    // `super(..)` where what the class extends makes the object. That is what the class object inherits from: what the declaration named, until somebody gives the class object another parent.
    bool resolveConstructSuper(BlockID block, unsigned index)
    {
        Value original = m_function.at(m_function.at(block).values[index]);
        LayoutID ofClass = classDefinedBy(original.child(0));
        if (!ofClass || m_asksClassObject[block.index()] == ofClass)
            return false;
        FunctionID parent = m_program.at(ofClass).declaredParentConstructor;
        if (!parent || !constructsOrdinaryObject(parent) || !m_program.at(parent).captureTypes.isEmpty())
            return false;
        auto& callee = m_program.at(parent);
        ValueID newTarget = original.child(1);
        bool isMadeBySuperclass = m_program.at(callee.constructedClass).hasForeignRoot;
        // Whoever makes the object reads the `prototype` of `new.target`, which runs no code if that is a closure of the program.
        if (!isMadeBySuperclass && m_function.at(originOf(newTarget)).opcode != Opcode::FunctionReference)
            return false;
        bool isAsDeclared = isKnownAsDeclared(block, index, ofClass);
        open(block, index);
        if (!isAsDeclared)
            askClassObjectUnlessAsDeclared(ofClass, original);
        ValueID made;
        if (isMadeBySuperclass)
            made = append(make(Opcode::Undefined, m_program.types.undefined));
        else {
            Value constructor = make(Opcode::FunctionReference, m_program.types.function);
            constructor.immediates[0] = parent.index();
            Value object = make(Opcode::CreateThis, m_program.types.object);
            object.children[0] = newTarget;
            object.children[1] = append(WTF::move(constructor));
            made = append(WTF::move(object));
        }
        Arguments given;
        if (callee.has(Format::FunctionHasNewTarget))
            given.append(newTarget);
        given.appendVector(original.arguments);
        ValueID returned = call(parent, passedTo(parent, { }, made, given.span()));
        m_function.at(returned).constructs = true;
        if (isMadeBySuperclass)
            made = returned;
        else if (mayReturnObject(callee)) {
            Value test = make(Opcode::CallBuiltin, m_program.types.boolean);
            test.immediates[0] = Operations::IsObject.index();
            test.arguments.append(returned);
            BlockID isNoObject = branch(append(WTF::move(test)));
            finish(held(returned, original.type, CastReason::Claim));
            moveTo(isNoObject);
        }
        finish(held(made, original.type, CastReason::Claim));
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

    // ConstructWith callee, object, arguments..: the front end has made the object, of the layout of the type of the `new` expression.
    bool resolveConstruct(BlockID block, unsigned index)
    {
        ValueID id = m_function.at(block).values[index];
        Value original = m_function.at(id);
        if (m_program.operationOf(original) != Operations::ConstructWith || original.arguments.size() < 2)
            return false;
        auto known = knownCallee(original.arguments[0]);
        if (!known || !mayConstructDirectly(known->function, true) || isRefuted(block, *known))
            return false;
        bool isOneOfMany = m_program.at(known->function).hasManyClosures;
        if (isOneOfMany && !m_program.genericEntrypoints.contains(known->function.index()))
            return false;
        ValueID object = original.arguments[1];
        if (m_function.at(object).opcode != Opcode::NewObjectWithShape || m_program.at(m_function.at(object).layout()).kind != LayoutKind::Record)
            return false;
        if (!m_program.at(known->function).thisType || !isWithin(object, m_program.at(known->function).thisType))
            return false;
        // Each way makes its own object, right before it hands it over. Nobody can tell when an object was made that nobody has seen.
        size_t birth = m_function.at(block).values.find(object);
        if (birth == notFound || birth >= index || !isUsedOnlyBy(object, id))
            return false;
        m_function.at(block).values.removeAt(birth);

        open(block, index - 1);
        BlockID otherwise;
        if (isOneOfMany) {
            takeCastOfResult(id);
            Value test = make(Opcode::IsClosureOf, m_program.types.boolean);
            test.children[0] = original.arguments[0];
            test.immediates = { known->function.index(), closureIsTheOneThatConstructs };
            otherwise = branch(append(WTF::move(test)));
            m_refuted[otherwise.index()] = Refuted { known->definition, known->function };
        } else if (!known->isCertain) {
            takeCastOfResult(id);
            otherwise = testClosure(original.arguments[0], *known);
        }
        Allocation allocation = m_function.allocations[m_function.at(object).allocation()];
        allocation.constructedBy = known->function;
        uint32_t number = m_function.allocationOf(WTF::move(allocation));
        ValueID made = otherwise ? addValue(Value(m_function.at(object))) : object;
        m_function.at(made).immediates[1] = number;
        Arguments arguments = passedTo(known->function, { }, made, original.arguments.span().subspan(2));
        place(made);
        call(known->function, WTF::move(arguments));
        finishWithCast(held(made, original.type, CastReason::Claim));
        if (otherwise) {
            moveTo(otherwise);
            place(object);
            bool isNil = known->isCertainUnlessNil && !isOneOfMany;
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

    // Construct callee (arguments..): `new` does not say what it makes, so the front end leaves the object to the run time: `new (F as any)()`,
    // a class that is the value of a variable. Where the callee is known the object is made here, as State::createThis() makes it.
    bool resolveConstructOfFunction(BlockID block, unsigned index)
    {
        ValueID id = m_function.at(block).values[index];
        Value original = m_function.at(id);
        auto known = knownCallee(original.child(0));
        if (!known || isRefuted(block, *known))
            return false;
        auto& callee = m_program.at(known->function);
        // Where a class on the way up has another parent, the `new` stays as it stands: see resolveConstructOfOrdinaryObject().
        if (LayoutID asked = m_asksClassObject[block.index()]; asked && classesWhoseSuperIsResolved(known->function).contains(asked))
            return false;
        Allocation allocation;
        if (mayConstructClassDirectly(known->function)) {
            allocation.layout = callee.constructedClass;
            setBornFields(m_program.at(allocation.layout), allocation);
        } else if (mayConstructDirectly(known->function) && callee.thisType) {
            allocation.layout = m_program.at(callee.thisType).onlyView();
            allocation.constructedBy = known->function;
            if (!allocation.layout || m_program.at(allocation.layout).kind != LayoutKind::Record || !m_program.at(allocation.layout).isAllocated)
                return false;
        } else if (constructsOrdinaryObject(known->function))
            return resolveConstructOfOrdinaryObject(block, index, *known);
        else
            return false;

        open(block, index);
        BlockID otherwise;
        if (!known->isCertain) {
            takeCastOfResult(id);
            otherwise = testClosure(original.child(0), *known);
        }
        Value object = make(Opcode::NewObjectWithShape, m_program.at(allocation.layout).type);
        object.immediates[0] = allocation.layout.index();
        object.immediates[1] = m_function.allocationOf(WTF::move(allocation));
        ValueID made = append(WTF::move(object));
        call(known->function, passedTo(known->function, { }, made, original.arguments.span()));
        finishWithCast(held(made, original.type, CastReason::Claim));
        if (otherwise) {
            moveTo(otherwise);
            if (known->isCertainUnlessNil)
                original.arguments.clear();
            ValueID general = appendThrowing(WTF::move(original));
            if (known->isCertainUnlessNil)
                finishThrowing();
            else
                finishWithCast(general);
        }
        return true;
    }

    const Program& m_program;
    Function& m_function;
    // Whether a call of what is only expected to be a closure of a function gets a test and a direct call.
    bool m_mayGuess;
    Vector<ValueID, 2> m_literalsNobodyElseHolds;
    StringID m_usingTakesObjects;
    StringID m_usingTakesDisposables;
    StringID m_symbol;
    StringID m_dispose;
    StringID m_typeError;
    StringID m_suppressedError;
    Vector<uint32_t> m_numberOfUses;
    unsigned m_numberOfBlocksAsRead { 0 };
    BitVector m_isReached;
    UncheckedKeyHashMap<unsigned, Vector<unsigned, 1>, IntHash<unsigned>, WTF::UnsignedWithZeroKeyHashTraits<unsigned>> m_unreachedPredecessors;
    Vector<unsigned> m_indexInRegion;
    // By block: the value, by its definition, that is known there not to be a closure of the function.
    Vector<Refuted> m_refuted;
    // By block: the class, where it is the arm of askClassObjectUnlessAsDeclared() that asks.
    Vector<LayoutID> m_asksClassObject;
    Vector<RefutedShape> m_refutedShapes;

    // Of the value that is being replaced.
    uint32_t m_position { 0 };
    uint32_t m_inlineFrame { 0 };
    bool m_isInTailPosition { false };
    // See inlineCalls().
    uint32_t m_inliningDepth { Format::none };
    bool m_isWatched { false };
    Target m_handler;
    BlockID m_after;
    // See takeCastOfResult().
    std::optional<Value> m_castOfResult;
    // Where values are added.
    BlockID m_block;
    bool m_lastMayThrow { false };
};

} // anonymous namespace

unsigned resolveCalls(const Program& program, Function& function, bool mayGuess, CleanUpTiming timing)
{
    return CallResolution(program, function, mayGuess).run(timing);
}

// The way in for callers that do not know the function: it takes the function value, `this` and the arguments as the engine passes
// them, holds them to the types of the parameters, and calls the function. That of a constructor is only ever entered by the run time,
// which has made `this` and passes `new.target` for the first argument if the constructor takes it: see State::createThis().
static Function createGenericEntrypoint(const Program& program, FunctionID id, FunctionID written = { })
{
    FunctionID secondBody = written ? program.at(id).secondBody : FunctionID();
    if (secondBody) {
        auto& served = program.at(id);
        auto& second = program.at(secondBody);
        if (second.captureTypes == served.captureTypes && second.parameterTypes.size() == served.parameterTypes.size() && second.has(Format::FunctionHasThis) == served.has(Format::FunctionHasThis) && !served.has(Format::FunctionHasRestParameter) && !served.takesItsClosure && !second.takesItsClosure)
            written = { };
        else
            secondBody = { };
    }
    FunctionID called = written ? written : program.at(id).rampForClosures ? program.at(id).rampForClosures : id;
    auto& callee = program.at(called);
    if (written) {
        auto& served = program.at(id);
        RELEASE_ASSERT(callee.kind == served.kind && callee.length == served.length && callee.captureTypes == served.captureTypes && callee.parameterTypes.size() == served.parameterTypes.size());
        RELEASE_ASSERT(callee.has(Format::FunctionHasThis) == served.has(Format::FunctionHasThis) && callee.has(Format::FunctionHasRestParameter) == served.has(Format::FunctionHasRestParameter) && !callee.takesItsClosure && !served.takesItsClosure);
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
    // What takes the rest of the arguments has to see them all, and so has what takes more than there are registers for.
    unsigned first = callee.takesItsClosure;
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
        if (!asserted && (!type || program.isHeldAlready(program.typeOf(entry, value), program.at(type), reason)))
            return value;
        Value cast = make(Opcode::Cast, type ? type : types.anything);
        cast.setAssertedType(asserted);
        cast.children[0] = value;
        cast.immediates[1] = static_cast<uint32_t>(reason);
        bool isOpenByNature = type && program.at(type).admitsForeignObjects(program) && !program.isOpenedForLog(program.at(type));
        if (!secondBody || asserted || !type || (reason == CastReason::As && !program.at(type).views.isEmpty() && !(program.logsTypeErrors && isOpenByNature)))
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
        // It is an array.
        if (TypeID declared = callee.parameterTypes[i]; isRest && declared && Type::engineClass(Builtins::singleton().arrayClass()).isWithin(program.at(declared), program))
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
    if (callee.takesItsClosure)
        all.append(closure);
    for (unsigned i = 0; i < given.size(); ++i) {
        // The array that is made here for the rest of the arguments is of the type as it is.
        bool isMadeHere = entry.at(given[i]).opcode == Opcode::CreateRest;
        all.append(held(given[i], callee.parameterTypes[first + i], isMadeHere ? TypeID() : callee.assertedTypeOfParameter(first + i), callee.reasonToHoldParameter(first + i)));
    }
    Value call = make(Opcode::CallDirect, callee.resultType ? callee.resultType : types.anything);
    call.immediates[0] = called.index();
    call.arguments = WTF::move(all);
    entry.blocks[current].terminator.kind = TerminatorKind::Return;
    entry.blocks[current].terminator.value = append(WTF::move(call));
    if (!isWatched)
        return entry;
    entry.flags |= FunctionLogsMisfits;
    current = 1;
    append(make(Opcode::CatchValue, types.anything));
    auto& second = program.at(secondBody);
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
    Value callOfSecondBody = make(Opcode::CallDirect, second.resultType ? second.resultType : types.anything);
    callOfSecondBody.immediates[0] = secondBody.index();
    callOfSecondBody.arguments = WTF::move(original);
    entry.blocks[1].terminator.kind = TerminatorKind::Return;
    entry.blocks[1].terminator.value = append(WTF::move(callOfSecondBody));
    return entry;
}

namespace {

struct SignatureOfValueEntry {
    Vector<TypeID, 4> parameters;
    TypeID result;
};

static bool isPlainFunctionWithKnownCallers(const Program& program, FunctionID id)
{
    auto& function = program.at(id);
    if (function.kind != FunctionKind::Plain && function.kind != FunctionKind::Arrow)
        return false;
    if (function.blocks.isEmpty() || function.isGenericEntrypoint() || function.isBodyOfSeveralSites || !function.resultTypes.isEmpty())
        return false;
    if (function.has(Format::FunctionHasRestParameter) || function.has(Format::FunctionHasNewTarget) || function.has(FunctionUsesArgumentBuffer) || function.has(FunctionHasUnknownCallers) || function.has(FunctionIsHalfOfHiddenCoroutine) || function.has(FunctionLogsMisfits) || function.has(FunctionIsEnteredByValue))
        return false;
    if (function.takesItsClosure || function.rampForClosures || function.isRampForClosures || function.writtenEntry || function.secondBody || function.resumeFunction || function.startFunction || function.worker || function.declaredAs & (functionIsAsync | functionIsGenerator))
        return false;
    return !program.isResumeFunction(function) && !program.isEntry(id);
}

static bool mayHaveValueEntry(const Program& program, FunctionID id)
{
    auto& function = program.at(id);
    if (!isPlainFunctionWithKnownCallers(program, id) || function.has(Format::FunctionIsBuiltin) || function.assertedThisType || !program.genericEntrypoints.contains(id.index()) || !program.makesSlimClosures(function))
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
        if (function.assertedTypeOfParameter(i))
            return false;
    }
    return std::ranges::none_of(function.values, [&](const Value& value) {
        if (value.opcode == Opcode::GetMyArgumentByVal)
            return value.readsArgumentsObject();
        return (value.opcode == Opcode::Operation || value.opcode == Opcode::CallBuiltin) && languageOperationOf(program, value) == LanguageOperation::CreateArguments;
    });
}

static Value makeValueOfEntry(const Program& program, Opcode opcode, TypeID type, uint32_t position)
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
    Value value = makeValueOfEntry(program, Opcode::Parameter, type, function.position);
    value.immediates = { 0, static_cast<uint32_t>(function.blocks[0].parameters.size()) };
    function.values.append(WTF::move(value));
    function.blocks[0].parameters.append(result);
    return result;
}

static Function createValueEntry(const Program& program, FunctionID id, const SignatureOfValueEntry& signature)
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
    entry.functionOfValueEntry = id;
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
        return append(makeValueOfEntry(program, Opcode::Undefined, program.types.undefined, callee.position));
    };
    auto held = [&](ValueID passed, TypeID type, CastReason reason) {
        if (!type || program.isHeldAlready(program.typeOf(entry, passed), program.at(type), reason))
            return passed;
        Value cast = makeValueOfEntry(program, Opcode::Cast, type, callee.position);
        cast.children[0] = passed;
        cast.immediates[1] = static_cast<uint32_t>(reason);
        return append(WTF::move(cast));
    };
    Arguments all;
    for (unsigned i = 0; i < callee.captureTypes.size(); ++i) {
        Value capture = makeValueOfEntry(program, Opcode::GetCapture, callee.captureTypes[i], callee.position);
        capture.children[0] = closure;
        capture.immediates = { id.index(), i };
        all.append(append(WTF::move(capture)));
    }
    if (callee.has(Format::FunctionHasThis))
        all.append(held(thisValue, callee.thisType, CastReason::Store));
    for (unsigned i = 0; i < callee.parameterTypes.size(); ++i)
        all.append(held(i < given.size() ? given[i] : undefinedValue(), callee.parameterTypes[i], callee.reasonToHoldParameter(i)));
    Value call = makeValueOfEntry(program, Opcode::CallDirect, callee.resultType ? callee.resultType : program.types.anything, callee.position);
    call.immediates[0] = id.index();
    call.arguments = WTF::move(all);
    entry.blocks[0].terminator.kind = TerminatorKind::Return;
    entry.blocks[0].terminator.value = append(WTF::move(call));
    return entry;
}

static bool mayHaveTwin(const Program& program, FunctionID id, const SignatureOfValueEntry& signature)
{
    static constexpr unsigned mostValuesOfTwin = 256;
    auto& callee = program.at(id);
    Rep rep = repOf(program.at(signature.result));
    if ((rep != Rep::Double && rep != Rep::OptionalDouble && rep != Rep::Int64 && rep != Rep::OptionalInt64) || !callee.resultType || repOf(program.at(callee.resultType)) != rep)
        return false;
    if (callee.values.size() > mostValuesOfTwin || callee.has(Format::FunctionHasThis) || callee.parameterTypes.size() < signature.parameters.size() || callee.blocks[0].parameters.size() != callee.captureTypes.size() + callee.parameterTypes.size())
        return false;
    if (std::ranges::any_of(callee.parameterTypes, [](TypeID type) { return !type; }))
        return false;
    return std::ranges::none_of(callee.values, [&](const Value& value) {
        return value.isInTailPosition || value.opcode == Opcode::NewClosure || value.opcode == Opcode::NewClosureOnce || value.opcode == Opcode::FunctionReference || (value.opcode == Opcode::CallDirect && value.function() == id);
    });
}

static Function createTwin(const Program& program, FunctionID id, const SignatureOfValueEntry& signature)
{
    Function twin = program.at(id);
    twin.isValueEntry = true;
    twin.isTwinOfItsFunction = true;
    twin.functionOfValueEntry = id;
    twin.takesCapturesFromClosure = !twin.captureTypes.isEmpty();
    auto& first = twin.blocks[0];
    unsigned numberOfCaptures = twin.captureTypes.size();
    unsigned numberAtHead = 0;
    while (twin.parameterTypes.size() > signature.parameters.size()) {
        ValueID parameter = first.parameters.takeLast();
        twin.parameterTypes.removeLast();
        twin.at(parameter) = makeValueOfEntry(program, Opcode::Undefined, program.types.undefined, twin.position);
        first.values.insert(numberAtHead++, parameter);
    }
    for (unsigned i = 0; i < signature.parameters.size(); ++i) {
        TypeID own = twin.parameterTypes[i];
        TypeID passed = signature.parameters[i];
        CastReason reason = twin.reasonToHoldParameter(i);
        if (repOf(program.at(own)) == repOf(program.at(passed)) && program.isHeldAlready(program.at(passed), program.at(own), reason))
            continue;
        ValueID parameter = first.parameters[numberOfCaptures + i];
        ValueID held(twin.values.size());
        auto takeHeld = [&](ValueID& child) {
            if (child == parameter)
                child = held;
        };
        for (auto& value : twin.values)
            value.forEachChild(takeHeld);
        for (auto& block : twin.blocks)
            block.terminator.forEachChild(takeHeld);
        Value cast = makeValueOfEntry(program, Opcode::Cast, own, twin.position);
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

static Function createAdapterOfSignature(const Program& program, const Function& like, const SignatureOfValueEntry& signature)
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
    Value call = makeValueOfEntry(program, Opcode::Call, signature.result, like.position);
    call.children[0] = callee;
    call.children[1] = thisValue;
    call.arguments = WTF::move(given);
    adapter.blocks[0].terminator.kind = TerminatorKind::Return;
    adapter.blocks[0].terminator.value = append(WTF::move(call));
    return adapter;
}

static uint32_t hashOfShape(std::span<const TypeID> parameters, TypeID result)
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

static bool mayGoBySignature(const Program& program, const Value& call)
{
    if (!call.hasCallSignature())
        return false;
    auto& declared = program.callSignatures[call.callSignature()];
    unsigned numberOfArguments = call.arguments.size();
    if (declared.hasRestParameter || numberOfArguments < declared.leastNumberOfArguments || numberOfArguments > declared.parameters.size())
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

static bool isDearToBox(const Type& type)
{
    Rep rep = repOf(type);
    return rep == Rep::Double || rep == Rep::OptionalDouble || rep == Rep::Int64 || rep == Rep::OptionalInt64;
}

static bool isPassedRaw(const Program& program, TypeID type) { return isDearToBox(program.at(type)); }

static Vector<TypeID, 4> parametersOfShape(const Program& program, const Value& call)
{
    Vector<TypeID, 4> parameters;
    parameters.append(program.callSignatures[call.callSignature()].parameters.span().first(call.arguments.size()));
    for (TypeID& type : parameters) {
        if (auto widened = program.numbersOrUndefined.find(type.index()); widened != program.numbersOrUndefined.end())
            type = widened->value;
    }
    return parameters;
}

static constexpr uint32_t bytesSparedAtCallByResult = 40;
static constexpr uint32_t bytesOfAdapterOfResult = 190;
static constexpr uint32_t leastCallsWorthAnAdapter = bytesOfAdapterOfResult / bytesSparedAtCallByResult + 1;
static constexpr unsigned maxResultsAskedFor = 64;

static Vector<uint32_t> numbersOfUsesIn(const Function& function)
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

static ValueID castBehindCall(const Function& function, const Block& block, unsigned index, const Vector<uint32_t>& numbersOfUses)
{
    ValueID id = block.values[index];
    if (function.at(id).isInTailPosition || index + 1 >= block.values.size() || numbersOfUses[id.index()] != 1)
        return { };
    auto& cast = function.at(block.values[index + 1]);
    if (cast.opcode != Opcode::Cast || cast.child(0) != id || cast.immediates[0] != Format::none || cast.assertedType() || cast.parameterCheckCallee())
        return { };
    if (cast.immediates[1] != static_cast<uint32_t>(CastReason::As) && cast.immediates[1] != static_cast<uint32_t>(CastReason::Store))
        return { };
    return block.values[index + 1];
}

static FunctionID adapterOfResultOfCall(const Program& program, const Function& function, const Value& call, ValueID cast)
{
    static_assert(numberOfArgumentGPRs < 256);
    TypeID result = cast ? function.at(cast).type : call.type;
    if (call.opcode != Opcode::Call || function.isValueEntry || !function.has(Format::FunctionIsTyped) || !result || call.arguments.size() > numberOfArgumentGPRs)
        return { };
    auto& type = program.at(result);
    if (type.isAnything() || type.isNever() || type.hasOnlyPrimitives(Primitive::Undefined) || isDearToBox(type))
        return { };
    return program.adaptersOfResults.get(Program::keyOfAdapterOfResult(result, cast ? function.at(cast).immediates[1] : Format::none, call.arguments.size()));
}

} // namespace

FunctionID Program::adapterOfCall(const Value& call) const
{
    if (shapesOfValueCalls.isEmpty() || call.opcode != Opcode::Call || !mayGoBySignature(*this, call))
        return { };
    auto parameters = parametersOfShape(*this, call);
    if (auto found = shapesOfValueCallsByHash.find(hashOfShape(parameters.span(), call.type)); found != shapesOfValueCallsByHash.end()) {
        for (uint32_t shape : found->value) {
            if (shapesOfValueCalls[shape].result == call.type && std::ranges::equal(shapesOfValueCalls[shape].parameters, parameters))
                return shapesOfValueCalls[shape].adapter;
        }
    }
    return { };
}

void createAdaptersOfResults(Program& program)
{
    if (program.mayRunInSeveralInstances || program.logsTypeErrors || Options::auditAOTTypedAssertions())
        return;
    auto& asRead = std::as_const(program);
    unsigned count = asRead.functions.size();
    struct Result {
        TypeID type;
        uint32_t castOfResult { Format::none };
        uint32_t like { 0 };
        uint32_t numberOfCalls { 0 };
    };
    struct Asked {
        TypeID type;
        uint32_t castOfResult;
        uint32_t numberOfArguments;
        uint32_t like;
        uint32_t numberOfCalls;
    };
    Vector<Asked> allAsked;
    UncheckedKeyHashMap<uint64_t, uint32_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> indexOfAsked;
    struct AskedAtCall {
        TypeID type;
        uint32_t castOfResult;
        uint32_t numberOfArguments;
    };
    Vector<Vector<AskedAtCall>> askedIn(count);
    forEachIndexInParallel(count, [&](unsigned i) {
        auto& function = asRead.functions[i];
        if (function.blocks.isEmpty() || function.isValueEntry || !function.has(Format::FunctionIsTyped) || std::ranges::none_of(function.values, [](auto& value) { return value.opcode == Opcode::Call; }))
            return;
        Vector<uint32_t> numbersOfUses = numbersOfUsesIn(function);
        for (auto& block : function.blocks) {
            for (unsigned indexInBlock = 0; indexInBlock < block.values.size(); ++indexInBlock) {
                auto& call = function.at(block.values[indexInBlock]);
                if (call.opcode != Opcode::Call || call.arguments.size() > numberOfArgumentGPRs || asRead.adapterOfCall(call))
                    continue;
                ValueID cast = castBehindCall(function, block, indexInBlock, numbersOfUses);
                TypeID type = cast ? function.at(cast).type : call.type;
                uint32_t castOfResult = cast ? function.at(cast).immediates[1] : Format::none;
                if (!type || asRead.at(type).isAnything() || asRead.at(type).isNever() || asRead.at(type).hasOnlyPrimitives(Primitive::Undefined) || isDearToBox(asRead.at(type)))
                    continue;
                askedIn[i].append(AskedAtCall { type, castOfResult, static_cast<uint32_t>(call.arguments.size()) });
            }
        }
    });
    for (unsigned i = 0; i < count; ++i) {
        for (auto [type, castOfResult, numberOfArguments] : askedIn[i]) {
            auto added = indexOfAsked.add(Program::keyOfAdapterOfResult(type, castOfResult, numberOfArguments), static_cast<uint32_t>(allAsked.size()));
            if (added.isNewEntry)
                allAsked.append(Asked { type, castOfResult, numberOfArguments, i, 0 });
            ++allAsked[added.iterator->value].numberOfCalls;
        }
    }
    Vector<Result> results;
    UncheckedKeyHashMap<uint64_t, uint32_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> indexOfResult;
    Vector<std::pair<uint32_t, uint32_t>> adaptersToMake;
    for (auto& asked : allAsked) {
        if (asked.numberOfCalls < leastCallsWorthAnAdapter)
            continue;
        auto added = indexOfResult.add(Program::keyOfAdapterOfResult(asked.type, asked.castOfResult, 0), static_cast<uint32_t>(results.size()));
        if (added.isNewEntry)
            results.append(Result { asked.type, asked.castOfResult, asked.like, 0 });
        results[added.iterator->value].numberOfCalls += asked.numberOfCalls;
        adaptersToMake.append({ added.iterator->value, asked.numberOfArguments });
    }
    if (results.isEmpty())
        return;

    Vector<std::pair<FunctionID, FunctionID>> candidates;
    Vector<TypeID> typesYielded;
    UncheckedKeyHashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> indexOfTypeYielded;
    for (auto& [index, general] : asRead.genericEntrypoints) {
        FunctionID id(index);
        auto& function = asRead.at(id);
        if (!isPlainFunctionWithKnownCallers(asRead, id) || function.has(Format::FunctionIsBuiltin) || !function.has(Format::FunctionIsTyped) || !function.resultType || !asRead.makesSlimClosures(function) || asRead.at(general).valueEntry)
            continue;
        candidates.append({ id, general });
    }
    std::ranges::sort(candidates, { }, [](auto& candidate) {
        return candidate.first.index();
    });
    for (auto [id, general] : candidates) {
        if (indexOfTypeYielded.add(asRead.at(id).resultType.index(), static_cast<uint32_t>(typesYielded.size())).isNewEntry)
            typesYielded.append(asRead.at(id).resultType);
    }
    Vector<uint32_t> mostAskedFirst(results.size(), [](size_t index) {
        return static_cast<uint32_t>(index);
    });
    std::ranges::sort(mostAskedFirst, [&](uint32_t a, uint32_t b) {
        return results[a].numberOfCalls != results[b].numberOfCalls ? results[a].numberOfCalls > results[b].numberOfCalls : a < b;
    });
    mostAskedFirst.shrink(std::min<size_t>(mostAskedFirst.size(), maxResultsAskedFor));
    Vector<uint64_t> resultsHolding(FillWith { }, typesYielded.size(), static_cast<uint64_t>(0));
    forEachIndexInParallel(typesYielded.size(), [&](unsigned k) {
        auto& yielded = asRead.at(typesYielded[k]);
        if (yielded.isNever())
            return;
        for (unsigned bit = 0; bit < mostAskedFirst.size(); ++bit) {
            auto& result = results[mostAskedFirst[bit]];
            if (result.castOfResult == Format::none ? yielded.isWithin(asRead.at(result.type), asRead) : asRead.isHeldAlready(yielded, asRead.at(result.type), static_cast<CastReason>(result.castOfResult)))
                resultsHolding[k] |= 1ULL << bit;
        }
    });
    uint64_t resultsHoldingSomething = 0;
    uint64_t numberOfFunctions = 0;
    for (auto [id, general] : candidates) {
        uint64_t holding = resultsHolding[indexOfTypeYielded.get(asRead.at(id).resultType.index())];
        if (!holding)
            continue;
        program.functions[general.index()].resultsHoldingWhatIsYielded = holding;
        program.functions[general.index()].typeYieldedWhenResultsWereCompared = asRead.at(id).resultType;
        resultsHoldingSomething |= holding;
        ++numberOfFunctions;
    }
    Vector<uint32_t> bitOfResult(FillWith { }, results.size(), static_cast<uint32_t>(Format::none));
    for (unsigned bit = 0; bit < mostAskedFirst.size(); ++bit) {
        if (resultsHoldingSomething >> bit & 1)
            bitOfResult[mostAskedFirst[bit]] = bit;
    }
    Vector<Function> made;
    for (auto [result, numberOfArguments] : adaptersToMake) {
        if (bitOfResult[result] == Format::none)
            continue;
        auto& like = asRead.functions[results[result].like];
        uint32_t castOfResult = results[result].castOfResult;
        SignatureOfValueEntry signature;
        signature.parameters.fill(asRead.types.anything, numberOfArguments);
        signature.result = castOfResult == Format::none ? results[result].type : asRead.types.anything;
        program.adaptersOfResults.set(Program::keyOfAdapterOfResult(results[result].type, castOfResult, numberOfArguments), FunctionID(count + made.size()));
        made.append(createAdapterOfSignature(asRead, like, signature));
        auto& adapter = made.last();
        if (castOfResult != Format::none) {
            Value cast = makeValueOfEntry(asRead, Opcode::Cast, results[result].type, like.position);
            cast.children[0] = adapter.blocks[0].terminator.value;
            cast.immediates[1] = castOfResult;
            adapter.blocks[0].terminator.value = ValueID(adapter.values.size());
            adapter.blocks[0].values.append(adapter.blocks[0].terminator.value);
            adapter.values.append(WTF::move(cast));
            adapter.resultType = results[result].type;
        }
        adapter.returnsBoxedValue = true;
        adapter.resultOfAdapter = bitOfResult[result];
    }
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT: calls of values by result: ", results.size(), " result types are asked for by ", leastCallsWorthAnAdapter, " calls or more as they were read; the ", mostAskedFirst.size(), " most asked are looked at: ", made.size(), " adapters; ", numberOfFunctions, " functions of ", candidates.size(), ", which yield ", typesYielded.size(), " types, yield what is within one of them");
    program.functions.appendVector(WTF::move(made));
}

void forgetResultsThatCallersCheck(Program& program)
{
    auto& asRead = std::as_const(program);
    for (auto& [index, general] : asRead.genericEntrypoints) {
        if (asRead.at(general).resultsHoldingWhatIsYielded && (asRead.functions[index].resultType != asRead.at(general).typeYieldedWhenResultsWereCompared || asRead.functions[index].isCalledFromDroppableFrame))
            program.functions[general.index()].resultsHoldingWhatIsYielded = 0;
    }
}

void createValueEntrypoints(Program& program)
{
    if (program.mayRunInSeveralInstances || program.logsTypeErrors || Options::auditAOTTypedAssertions() || program.callSignatures.isEmpty())
        return;
    auto& asRead = std::as_const(program);
    unsigned count = asRead.functions.size();
    struct Shape {
        SignatureOfValueEntry signature;
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
            Type type = asRead.at(id);
            type.primitives |= Primitive::Undefined;
            typesOrUndefined[id.index()] = program.types.intern(type);
        }
        return typesOrUndefined[id.index()];
    };
    for (auto& signature : asRead.callSignatures) {
        for (TypeID type : signature.parameters) {
            if (type && asRead.at(type).isNumber())
                program.numbersOrUndefined.add(type.index(), orUndefined(type));
        }
    }
    struct Site {
        Vector<TypeID, 4> parameters;
        TypeID result;
    };
    Vector<Vector<Site>> sitesIn(count);
    forEachIndexInParallel(count, [&](unsigned i) {
        auto& function = asRead.functions[i];
        if (function.blocks.isEmpty() || !function.has(Format::FunctionIsTyped))
            return;
        for (auto& call : function.values) {
            if (!mayGoBySignature(asRead, call))
                continue;
            auto parameters = parametersOfShape(asRead, call);
            if (!isPassedRaw(asRead, call.type) && std::ranges::none_of(parameters, [&](TypeID type) { return isPassedRaw(asRead, type); }))
                continue;
            if (asRead.at(call.type).hasCells() || std::ranges::any_of(parameters, [&](TypeID type) { return asRead.at(type).hasCells(); }))
                continue;
            sitesIn[i].append(Site { WTF::move(parameters), call.type });
        }
    });
    for (unsigned i = 0; i < count; ++i) {
        for (auto& [parameters, result] : sitesIn[i]) {
            auto& alike = shapesByHash.add(hashOfShape(parameters.span(), result), Vector<uint32_t, 1>()).iterator->value;
            uint32_t shape = std::numeric_limits<uint32_t>::max();
            for (uint32_t known : alike) {
                if (shapes[known].signature.result == result && std::ranges::equal(shapes[known].signature.parameters, parameters))
                    shape = known;
            }
            if (shape == std::numeric_limits<uint32_t>::max()) {
                shape = shapes.size();
                alike.append(shape);
                SignatureOfValueEntry signature;
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
        unsigned numberGiven { 0 };
    };
    Vector<FunctionID> enteredByValue;
    for (auto& [index, general] : asRead.genericEntrypoints)
        enteredByValue.append(FunctionID(index));
    std::ranges::sort(enteredByValue, { }, [](FunctionID id) {
        return id.index();
    });
    Vector<uint8_t> mayHaveEntry(FillWith { }, enteredByValue.size(), static_cast<uint8_t>(0));
    forEachIndexInParallel(enteredByValue.size(), [&](unsigned k) {
        mayHaveEntry[k] = mayHaveValueEntry(asRead, enteredByValue[k]);
    });
    Vector<FunctionID> candidates;
    for (unsigned k = 0; k < enteredByValue.size(); ++k) {
        if (mayHaveEntry[k])
            candidates.append(enteredByValue[k]);
    }
    Vector<Vector<TypeID, 4>> whatParametersMayBeGiven(candidates.size());
    for (unsigned k = 0; k < candidates.size(); ++k) {
        for (TypeID type : asRead.at(candidates[k]).parameterTypes)
            whatParametersMayBeGiven[k].append(!type ? asRead.types.anything : asRead.at(type).isNumber() ? orUndefined(type) : type);
    }
    Vector<uint32_t> shapesByNumberOfSites(shapes.size(), [](size_t index) {
        return static_cast<uint32_t>(index);
    });
    std::ranges::sort(shapesByNumberOfSites, [&](uint32_t a, uint32_t b) {
        return shapes[a].numberOfSites != shapes[b].numberOfSites ? shapes[a].numberOfSites > shapes[b].numberOfSites : a < b;
    });
    Vector<Answer> answersOfCandidates(candidates.size());
    forEachIndexInParallel(candidates.size(), [&](unsigned k) {
        auto& function = asRead.at(candidates[k]);
        auto& own = whatParametersMayBeGiven[k];
        unsigned leastGiven = own.size();
        while (leastGiven && asRead.at(asRead.types.undefined).isWithin(asRead.at(function.parameterTypes[leastGiven - 1] ? function.parameterTypes[leastGiven - 1] : asRead.types.anything), asRead))
            --leastGiven;
        auto& yielded = asRead.at(function.resultType ? function.resultType : asRead.types.anything);
        auto& answer = answersOfCandidates[k];
        answer.function = candidates[k];
        for (unsigned round = 0; round < 2 * shapesByNumberOfSites.size(); ++round) {
            uint32_t shape = shapesByNumberOfSites[round % shapesByNumberOfSites.size()];
            auto& signature = shapes[shape].signature;
            unsigned given = std::min<unsigned>(signature.parameters.size(), own.size());
            if (given < leastGiven || !yielded.isWithin(asRead.at(signature.result), asRead))
                continue;
            if (round < shapesByNumberOfSites.size() && repOf(asRead.at(signature.result)) != repOf(yielded))
                continue;
            bool fits = true;
            for (unsigned i = 0; i < given && fits; ++i)
                fits = !asRead.at(own[i]).hasCells() && (signature.parameters[i] == own[i] || asRead.at(signature.parameters[i]).isWithin(asRead.at(own[i]), asRead));
            if (!fits)
                continue;
            bool crosses = isPassedRaw(asRead, signature.result) && isDearToBox(yielded);
            for (unsigned i = 0; i < given && !crosses; ++i)
                crosses = isPassedRaw(asRead, signature.parameters[i]);
            if (!crosses)
                continue;
            answer.shape = shape;
            answer.numberGiven = given;
            break;
        }
    });
    Vector<Answer> answers;
    for (auto& answer : answersOfCandidates) {
        if (answer.shape != Format::none)
            answers.append(answer);
    }
    if (answers.isEmpty())
        return;
    BitVector answersSomeShape(count);
    for (auto& answer : answers)
        answersSomeShape.set(answer.function.index());
    Vector<uint8_t> isHandedToTheProgram(FillWith { }, count, static_cast<uint8_t>(0));
    for (auto& layout : asRead.layouts) {
        for (auto* methods : { &layout.methods, &layout.staticMethods }) {
            for (auto& method : *methods) {
                if (method.function)
                    isHandedToTheProgram[method.function.index()] = 1;
            }
        }
    }
    forEachIndexInParallel(count, [&](unsigned i) {
        auto& function = asRead.functions[i];
        Vector<ValueID, 8> closures;
        for (auto& block : function.blocks) {
            for (ValueID id : block.values) {
                auto& value = function.at(id);
                if ((value.opcode == Opcode::FunctionReference || value.opcode == Opcode::NewClosure || value.opcode == Opcode::NewClosureOnce) && answersSomeShape.get(value.function().index()))
                    closures.append(id);
            }
        }
        if (closures.isEmpty())
            return;
        auto isHanded = [&](ValueID used) {
            if (closures.contains(used))
                WTF::atomicStore(&isHandedToTheProgram[function.at(used).function().index()], static_cast<uint8_t>(1), std::memory_order_relaxed);
        };
        for (auto& block : function.blocks) {
            for (ValueID id : block.values) {
                auto& value = function.at(id);
                if (value.opcode == Opcode::Call) {
                    auto& callee = function.at(value.child(0));
                    if (callee.opcode == Opcode::GetGlobal)
                        continue;
                    if (callee.opcode == Opcode::GetById && value.child(1) && callee.child(0) == value.child(1)) {
                        auto& receiver = asRead.typeOf(function, value.child(1));
                        if (!receiver.anyObject && !receiver.anyFunction && receiver.views.isEmpty())
                            continue;
                    }
                    if (value.child(1))
                        isHanded(value.child(1));
                    for (ValueID argument : value.arguments)
                        isHanded(argument);
                    continue;
                }
                if (value.opcode == Opcode::CallMethod) {
                    auto& receiver = asRead.typeOf(function, value.child(0));
                    if (!receiver.anyObject && !receiver.anyFunction && receiver.views.isEmpty())
                        continue;
                }
                value.forEachChild(isHanded);
            }
            if (block.terminator.value)
                isHanded(block.terminator.value);
            for (auto& target : block.terminator.targets) {
                for (ValueID argument : target.arguments)
                    isHanded(argument);
            }
        }
    });
    answers.removeAllMatching([&](auto& answer) {
        return !isHandedToTheProgram[answer.function.index()];
    });
    if (answers.isEmpty())
        return;

    Vector<Function> made;
    for (auto& answer : answers) {
        if (shapes[answer.shape].adapter)
            continue;
        shapes[answer.shape].adapter = FunctionID(count + made.size());
        made.append(createAdapterOfSignature(asRead, asRead.functions[shapes[answer.shape].like], shapes[answer.shape].signature));
    }
    size_t numberOfAdapters = made.size();
    program.firstAdapterOfSignature = FunctionID(count);
    made.grow(numberOfAdapters + answers.size());
    forEachIndexInParallel(answers.size(), [&](unsigned k) {
        auto& answer = answers[k];
        auto& shape = shapes[answer.shape];
        SignatureOfValueEntry signature;
        signature.parameters.append(shape.signature.parameters.span().first(answer.numberGiven));
        signature.result = shape.signature.result;
        auto& entry = made[numberOfAdapters + k];
        entry = mayHaveTwin(asRead, answer.function, signature) ? createTwin(asRead, answer.function, signature) : createValueEntry(asRead, answer.function, signature);
        entry.adapterOfValueEntry = shape.adapter;
    });
    for (unsigned k = 0; k < answers.size(); ++k)
        program.functions[asRead.genericEntrypoints.get(answers[k].function.index()).index()].valueEntry = FunctionID(count + numberOfAdapters + k);
    for (auto& shape : shapes) {
        if (!shape.adapter)
            continue;
        program.shapesOfValueCallsByHash.add(hashOfShape(shape.signature.parameters.span(), shape.signature.result), Vector<uint32_t, 1>()).iterator->value.append(program.shapesOfValueCalls.size());
        program.shapesOfValueCalls.append({ shape.signature.parameters, shape.signature.result, shape.adapter });
    }
    program.functions.appendVector(WTF::move(made));
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT: calls of values by signature: ", shapes.size(), " signatures through which a double crosses at the calls as they were read; ", answers.size(), " functions of ", candidates.size(), " answer ", numberOfAdapters, " of them");
}

void callValuesBySignature(Program& program)
{
    if (program.shapesOfValueCalls.isEmpty() && program.adaptersOfResults.isEmpty())
        return;
    auto& asRead = std::as_const(program);
    uint64_t numberOfSites = 0;
    uint64_t numberLeft = 0;
    uint64_t numberCheapToBox = 0;
    struct CallByResult {
        uint32_t function;
        ValueID call;
        ValueID cast;
        FunctionID adapter;
    };
    Vector<CallByResult> callsByResult;
    UncheckedKeyHashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> numberOfCallsOfAdapter;
    enum class Verdict : uint8_t { IsLeft, GoesByResult, IsCheapToBox, GoesBySignature };
    struct Judged {
        ValueID call;
        ValueID cast;
        FunctionID adapter;
        Verdict verdict;
    };
    Vector<Vector<Judged>> judgedIn(asRead.functions.size());
    forEachIndexInParallel(asRead.functions.size(), [&](unsigned i) {
        auto& function = asRead.functions[i];
        if (function.blocks.isEmpty() || function.isValueEntry || std::ranges::none_of(function.values, [](auto& value) { return value.opcode == Opcode::Call; }))
            return;
        Vector<uint32_t> numbersOfUses;
        if (!asRead.adaptersOfResults.isEmpty())
            numbersOfUses = numbersOfUsesIn(function);
        for (auto& block : function.blocks) {
            for (unsigned indexInBlock = 0; indexInBlock < block.values.size(); ++indexInBlock) {
                ValueID id = block.values[indexInBlock];
                auto& call = function.at(id);
                if (call.opcode != Opcode::Call)
                    continue;
                FunctionID adapter = asRead.adapterOfCall(call);
                ValueID cast;
                if (!adapter && !asRead.adaptersOfResults.isEmpty()) {
                    cast = castBehindCall(function, block, indexInBlock, numbersOfUses);
                    adapter = adapterOfResultOfCall(asRead, function, call, cast);
                }
                if (!adapter || hasNoBody(asRead.at(adapter)))
                    continue;
                ValueID origin = call.child(0);
                while (function.at(origin).opcode == Opcode::Cast)
                    origin = function.at(origin).child(0);
                bool goes = !call.assertedType() && call.methodAskedByStub == MethodAskedByStub::None && !function.at(call.child(0)).predictedClosure() && !function.at(origin).predictedClosure();
                if (LayoutID view = call.child(1) ? asRead.typeOf(function, call.child(1)).onlyView() : LayoutID(); view && asRead.at(view).mayBeInFrame)
                    goes = false;
                auto& parameters = asRead.at(adapter).blocks[0].parameters;
                for (unsigned k = 0; k < call.arguments.size() && goes; ++k) {
                    auto& given = asRead.typeOf(function, call.arguments[k]);
                    LayoutID view = given.onlyView();
                    goes = given.isWithin(asRead.typeOf(asRead.at(adapter), parameters[k + 2]), asRead) && !(view && asRead.at(view).mayBeInFrame);
                }
                if (!goes) {
                    judgedIn[i].append(Judged { id, cast, adapter, Verdict::IsLeft });
                    continue;
                }
                if (asRead.at(adapter).resultOfAdapter != Format::none) {
                    judgedIn[i].append(Judged { id, cast, adapter, Verdict::GoesByResult });
                    continue;
                }
                bool passesWhatIsDearToBox = isPassedRaw(asRead, call.type);
                for (unsigned k = 0; k < call.arguments.size() && !passesWhatIsDearToBox; ++k)
                    passesWhatIsDearToBox = isDearToBox(asRead.typeOf(function, call.arguments[k]));
                judgedIn[i].append(Judged { id, cast, adapter, passesWhatIsDearToBox ? Verdict::GoesBySignature : Verdict::IsCheapToBox });
            }
        }
    });
    for (unsigned i = 0; i < asRead.functions.size(); ++i) {
        for (auto [id, cast, adapter, verdict] : judgedIn[i]) {
            switch (verdict) {
            case Verdict::IsLeft:
                ++numberLeft;
                break;
            case Verdict::GoesByResult:
                callsByResult.append({ i, id, cast, adapter });
                ++numberOfCallsOfAdapter.add(adapter.index(), 0).iterator->value;
                break;
            case Verdict::IsCheapToBox:
                ++numberCheapToBox;
                break;
            case Verdict::GoesBySignature: {
                auto& changed = program.functions[i].at(id);
                changed.goesBySignature = true;
                changed.immediates[1] = adapter.index();
                program.adaptersCalledBySignature.set(adapter.index());
                ++numberOfSites;
                break;
            }
            }
        }
    }
    uint64_t numberOfSitesByResult = 0;
    uint64_t numberOfCastsSpared = 0;
    uint64_t numberOfAdaptersOfResults = 0;
    uint64_t resultsWorthIt = 0;
    int64_t bytesSparedByUpperHalf = 0;
    for (auto& [adapter, numberOfCalls] : numberOfCallsOfAdapter) {
        if (numberOfCalls < leastCallsWorthAnAdapter)
            continue;
        uint32_t result = asRead.functions[adapter].resultOfAdapter;
        resultsWorthIt |= 1ULL << result;
        if (result >= 32)
            bytesSparedByUpperHalf += static_cast<int64_t>(numberOfCalls) * bytesSparedAtCallByResult - bytesOfAdapterOfResult;
    }
    int64_t numberOfEntriesThatSaySomething = 0;
    for (auto& [index, general] : asRead.genericEntrypoints)
        numberOfEntriesThatSaySomething += !!(asRead.at(general).resultsHoldingWhatIsYielded & resultsWorthIt);
    program.sizeOfResultsHoldingWhatIsYielded = sizeof(uint64_t);
    if (bytesSparedByUpperHalf <= numberOfEntriesThatSaySomething * static_cast<int64_t>(sizeof(uint32_t))) {
        program.sizeOfResultsHoldingWhatIsYielded = sizeof(uint32_t);
        resultsWorthIt &= std::numeric_limits<uint32_t>::max();
    }
    for (auto& [function, id, cast, adapter] : callsByResult) {
        if (numberOfCallsOfAdapter.get(adapter.index()) < leastCallsWorthAnAdapter || !(resultsWorthIt >> asRead.at(adapter).resultOfAdapter & 1))
            continue;
        auto& changed = program.functions[function].at(id);
        changed.goesBySignature = true;
        changed.immediates[1] = adapter.index();
        if (cast) {
            changed.type = asRead.functions[function].at(cast).type;
            changed.rep = repOf(asRead.at(changed.type));
            program.functions[function].at(cast).setCastReason(CastReason::Upcast);
            ++numberOfCastsSpared;
        }
        ++numberOfSitesByResult;
        if (program.adaptersCalledBySignature.get(adapter.index()))
            continue;
        program.adaptersCalledBySignature.set(adapter.index());
        ++numberOfAdaptersOfResults;
        program.resultsThatCallsGoBy |= 1ULL << asRead.at(adapter).resultOfAdapter;
    }
    program.callsBySignatureAreChosen = true;
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT: calls of values by signature: ", numberOfSites, " calls go so; ", numberLeft, " could, and an argument is not proven or a function is expected; ", numberCheapToBox, " pass nothing that is dear to box");
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT: calls of values by result: ", numberOfSitesByResult, " calls of ", callsByResult.size(), " go so, ", numberOfCastsSpared, " of them with the cast behind them, by ", numberOfAdaptersOfResults, " adapters, for ", std::popcount(program.resultsThatCallsGoBy), " result types; an adapter with fewer than ", leastCallsWorthAnAdapter, " calls is not worth its bytes; ", program.sizeOfResultsHoldingWhatIsYielded, " bytes lie ahead of a general entry that says something");
}

void findDeclaredParents(Program& program)
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
        for (auto& value : function.values) {
            if (value.opcode != Opcode::Operation || program.operationOf(value) != Operations::MakeClass || value.arguments.size() != 2)
                continue;
            FunctionID constructor = constantClosure(value.arguments[0]);
            FunctionID parent = constantClosure(value.arguments[1]);
            if (constructor && std::as_const(program).at(constructor).constructedClass && function.at(withoutCasts(value.arguments[1])).opcode == Opcode::GetGlobal)
                found[i].append({ std::as_const(program).at(constructor).constructedClass, FunctionID() });
            if (!constructor || !parent || std::as_const(program).at(parent).kind != FunctionKind::Constructor)
                continue;
            if (LayoutID ofClass = std::as_const(program).at(constructor).constructedClass; ofClass && program.at(ofClass).isEvaluatedOnce && !program.at(ofClass).base)
                found[i].append({ ofClass, parent });
        }
    });
    for (auto& ofFunction : found) {
        for (auto [ofClass, parent] : ofFunction) {
            if (parent)
                program.at(ofClass).declaredParentConstructor = parent;
            else
                program.at(ofClass).isDeclaredToExtendGlobal = true;
        }
    }
}

void createGenericEntrypoints(Program& program)
{
    for (unsigned i = 0; i < program.layouts.size(); ++i) {
        if (!program.layouts[i].type)
            program.layouts[i].type = program.types.intern(Type::view(LayoutID(i)));
    }
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        for (auto& value : std::as_const(program).functions[i].values) {
            if (value.opcode == Opcode::NewClosure || value.opcode == Opcode::NewClosureOnce)
                WTF::atomicStore(&program.at(value.function()).hasManyClosures, true, std::memory_order_relaxed);
        }
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

    // The run time makes a closure of the constructor of every class, which is the class object, and of every method.
    BitVector isValue(program.functions.size());
    BitVector isMethodOfClass(program.functions.size());
    for (unsigned i = 0; i < program.functions.size(); ++i) {
        if (std::as_const(program).functions[i].has(Format::FunctionIsUsedAsValue))
            isValue.set(i);
    }
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
    for (unsigned i = 0; i < program.functions.size(); ++i) {
        if (!std::as_const(program).functions[i].has(Format::FunctionIsGeneralEntry))
            continue;
        bool servesMethodOfClass = false;
        for (auto& value : std::as_const(program).functions[i].values) {
            if (value.opcode != Opcode::CallDirect)
                continue;
            servesMethodOfClass = isMethodOfClass.get(value.function().index());
            break;
        }
        bool isWrittenForLog = program.logsTypeErrors && !servesMethodOfClass;
        if (isWrittenForLog)
            program.functions[i].flags |= FunctionLogsMisfits;
        FunctionID served;
        for (auto& value : std::as_const(program).functions[i].values) {
            if (value.opcode != Opcode::CallDirect)
                continue;
            if (served) {
                program.functions[served.index()].secondBody = value.function();
                break;
            }
            auto& entry = std::as_const(program).functions[i];
            auto& candidate = std::as_const(program).at(value.function());
            bool fits = !isWrittenForLog || (entry.kind == candidate.kind && entry.length == candidate.length && entry.captureTypes == candidate.captureTypes && entry.parameterTypes.size() == candidate.parameterTypes.size()
                && entry.has(Format::FunctionHasThis) == candidate.has(Format::FunctionHasThis) && entry.has(Format::FunctionHasRestParameter) == candidate.has(Format::FunctionHasRestParameter) && !entry.takesItsClosure && !candidate.takesItsClosure);
            if (fits && isValue.get(value.function().index()) && !candidate.rampForClosures) {
                program.functions[value.function().index()].writtenEntry = FunctionID(i);
                served = value.function();
            }
            if (!served || !isWrittenForLog)
                break;
        }
    }
    for (unsigned i = 0, numberOfFunctionsAsRead = program.functions.size(); i < numberOfFunctionsAsRead; ++i) {
        FunctionID written = std::as_const(program).functions[i].writtenEntry;
        if (!written || !std::as_const(program).at(written).has(FunctionLogsMisfits))
            continue;
        Function copy = std::as_const(program).at(written);
        copy.flags |= FunctionIsEnteredByValue;
        program.functions[i].writtenEntry = FunctionID(program.functions.size());
        program.functions.append(WTF::move(copy));
    }
    Vector<unsigned> entered;
    for (unsigned i : isValue) {
        if (!std::as_const(program).functions[i].isGenericEntrypoint() && !program.genericEntrypoints.contains(i))
            entered.append(i);
    }
    unsigned firstEntry = program.functions.size();
    program.functions.grow(firstEntry + entered.size());
    program.firstGenericEntrypoint = firstEntry;
    program.numberOfConsecutiveGenericEntrypoints = entered.size();
    forEachIndexInParallel(entered.size(), [&](unsigned k) {
        program.functions[firstEntry + k] = createGenericEntrypoint(program, FunctionID(entered[k]), program.functions[entered[k]].writtenEntry);
    });
    for (unsigned k = 0; k < entered.size(); ++k)
        program.genericEntrypoints.add(entered[k], FunctionID(firstEntry + k));
}

// It takes what the function captures, `this` where the function takes one, the arguments that are bound, the closure that is bound unless the function has one closure, and the parameters that are left, each as the function declares it.
static Function createBoundFunction(const Program& program, FunctionID id, unsigned numberOfBoundArguments, StringID name)
{
    auto& callee = program.at(id);
    auto& types = program.types;
    // More than the function takes: it does not look at them.
    unsigned taken = std::min<unsigned>(numberOfBoundArguments, callee.parameterTypes.size());
    Function bound;
    bound.name = name;
    bound.kind = FunctionKind::Bound;
    bound.module = callee.module;
    bound.position = callee.position;
    bound.length = callee.length > numberOfBoundArguments ? callee.length - numberOfBoundArguments : 0;
    bound.flags = Format::FunctionIsUsedAsValue | Format::FunctionIsHiddenInStackTraces | (callee.flags & (Format::FunctionIsTyped | Format::FunctionIsOfBuiltinModule));
    // It inherits from what the function inherits from, which is all that "async" says of it.
    bound.declaredAs = callee.declaredAs & (functionIsOfBuiltinModule | functionIsAsync);
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
        if (callee.assertedTypeOfParameter(i)) {
            bound.assertedParameterTypes.grow(i - taken + 1);
            bound.assertedParameterTypes[i - taken] = callee.assertedTypeOfParameter(i);
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
        CallResolution(program, program.functions[i]).expandDisposableStacks();
    });
}

void createBoundFunctions(Program& program)
{
    Vector<Vector<uint64_t>> wantedBy(program.functions.size());
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        auto& function = program.functions[i];
        if (!function.isGenericEntrypoint())
            CallResolution(program, function).collectBoundFunctions(wantedBy[i]);
    });
    Vector<uint64_t> wanted;
    for (auto& ofFunction : wantedBy)
        wanted.appendVector(ofFunction);
    // In the order of the program, whatever the table makes of them.
    for (uint64_t key : wanted) {
        if (program.boundFunctions.contains(key))
            continue;
        FunctionID function(static_cast<uint32_t>(key >> 2));
        Vector<uint8_t> name;
        name.append(asBytes("bound "_s.span8()));
        name.append(program.strings[std::as_const(program).at(function).name.index()]);
        StringID nameOfBound = program.intern(name.span());
        FunctionID bound(program.functions.size());
        program.functions.append(createBoundFunction(program, function, key & 3, nameOfBound));
        program.boundFunctions.add(key, bound);
        Function entry = createGenericEntrypoint(program, bound);
        program.genericEntrypoints.add(bound.index(), FunctionID(program.functions.size()));
        program.functions.append(WTF::move(entry));
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
