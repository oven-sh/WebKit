/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedPasses.h"

#if ENABLE(AOT)

#include "AOTConvention.h"
#include <wtf/BitVector.h>

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
const Function* rampBehind(const Program& program, const Function& callee)
{
    if (callee.startFunction)
        return &callee;
    if (callee.kind != FunctionKind::Bound || callee.blocks.isEmpty() || callee.blocks[0].values.isEmpty())
        return nullptr;
    auto& call = callee.at(callee.blocks[0].values.last());
    return call.opcode == Opcode::CallDirect && program.at(call.function()).startFunction ? &program.at(call.function()) : nullptr;
}

// Of what an async function hands back. Nothing: it is none.
TypeID typeOfPromiseOf(const Program& program, const Function& callee)
{
    auto* ramp = rampBehind(program, callee);
    auto newPromise = program.find("NewPromise"_s.span8());
    if (!ramp || !newPromise || !program.find("RejectPromise"_s.span8()))
        return { };
    for (auto& value : ramp->values) {
        if (value.opcode == Opcode::Operation && value.string() == *newPromise)
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
        , m_constructWith(program.find("ConstructWith"_s.span8()).value_or(StringID()))
        , m_superGet(program.find("SuperGet"_s.span8()).value_or(StringID()))
        , m_superSet(program.find("SuperSet"_s.span8()).value_or(StringID()))
        , m_apply(Builtins::singleton().find(BuiltinKind::Operation, { }, Builtins::singleton().nameOf("Apply"_s.span8())))
        , m_isObject(Builtins::singleton().find(BuiltinKind::Operation, { }, Builtins::singleton().nameOf("IsObject"_s.span8())))
        , m_callNoInline(Builtins::singleton().find(BuiltinKind::Operation, { }, Builtins::singleton().nameOf("CallNoInline"_s.span8())))
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

    unsigned run()
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
        if (resolved)
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
    };

    using Refuted = std::pair<ValueID, FunctionID>;

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
            return resolveByShape(block, index, MethodKind::Method) || resolveOnClassObject(block, index) || resolveCallOrApply(block, index) || resolveBind(block, index) || resolveCallOfField(block, index);
        case Opcode::GetById:
            return resolveByShape(block, index, MethodKind::Getter) || resolveOnClassObject(block, index);
        case Opcode::PutById:
            return resolveByShape(block, index, MethodKind::Setter) || resolveOnClassObject(block, index);
        case Opcode::Call:
            return resolveCall(block, index) || resolveCallOfMethodRead(block, index) || resolveCallOrApply(block, index);
        case Opcode::Operation:
            return resolveConstruct(block, index) || resolveSuper(block, index) || resolveCallNoInline(block, index);
        case Opcode::Construct:
            return resolveConstructOfFunction(block, index);
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
        return BlockID(m_function.blocks.size() - 1);
    }

    Value make(Opcode opcode, TypeID type) const
    {
        Value value;
        value.opcode = opcode;
        value.position = m_position;
        value.inlineFrame = m_inlineFrame;
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
        return appendThrowing(WTF::move(cast));
    }

    // Calls of a function that is known.

    bool mayBeCalledDirectly(FunctionID id) const
    {
        auto& callee = m_program.at(id);
        if (callee.blocks.isEmpty() || callee.isGenericEntrypoint() || callee.takesItsClosure)
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
            all.append(held(thisValue, callee.thisType, CastReason::Flow));
        unsigned count = callee.parameterTypes.size();
        for (unsigned k = 0; k < count; ++k) {
            TypeID type = callee.parameterTypes[k];
            if (callee.has(Format::FunctionHasRestParameter) && k + 1 == count) {
                Value rest = make(Opcode::NewArray, type ? type : m_program.types.anything);
                if (k < given.size())
                    rest.arguments.append(given.subspan(k));
                all.append(appendThrowing(WTF::move(rest)));
            } else if (k < given.size())
                all.append(held(given[k], type, callee.reasonToHoldParameter(k)));
            else
                all.append(held(append(make(Opcode::Undefined, m_program.types.undefined)), type, callee.reasonToHoldParameter(k)));
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

    // `type`: of the value that is replaced.
    void callAndFinish(FunctionID id, std::span<const ValueID> captures, ValueID thisValue, std::span<const ValueID> given, TypeID type, Yields yields)
    {
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
        newPromise.immediates[0] = m_program.find("NewPromise"_s.span8())->index();
        ValueID promise = append(WTF::move(newPromise));
        Value reject = make(Opcode::Operation, m_program.types.undefined);
        reject.immediates[0] = m_program.find("RejectPromise"_s.span8())->index();
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
    std::optional<Cases> casesOf(LayoutID view, StringID name, MethodKind kind) const
    {
        // The receiver may be any object. The lowering tells those of the members from the rest, and asks the rest.
        if (m_program.at(view).admitsForeignObjects)
            return std::nullopt;
        Cases cases;
        for (LayoutID member : m_program.at(view).members) {
            auto& layout = m_program.at(member);
            FunctionID callee;
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

    bool resolveByShape(BlockID block, unsigned index, MethodKind kind)
    {
        Value original = m_function.at(m_function.at(block).values[index]);
        ValueID object = original.child(0);
        StringID name = original.string();
        LayoutID view = typeOf(object).onlyView();
        if (!view || !isObjectLayout(m_program.at(view).kind))
            return false;
        auto cases = casesOf(view, name, kind);
        if (!cases || !orderCases(*cases, view))
            return false;
        Arguments given;
        if (kind == MethodKind::Method)
            given = original.arguments;
        else if (kind == MethodKind::Setter)
            given.append(original.child(1));

        open(block, index);
        ValueID receiver = object;
        // The read of a field sees to it itself.
        if ((*cases)[0].callee && typeOf(object).mayBeUndefinedOrNull()) {
            Value cast = make(Opcode::Cast, m_program.at(view).type);
            cast.children[0] = object;
            cast.immediates = { name.index(), static_cast<uint32_t>(CastReason::Flow) };
            receiver = appendThrowing(WTF::move(cast));
        }
        dispatch(*cases, view, receiver, given.span(), original, kind == MethodKind::Setter ? Yields::Undefined : Yields::Result);
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
        if (originOf(object) != originOf(original.child(1)) || !view || !isObjectLayout(m_program.at(view).kind))
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
        call.immediates[1] = m_inliningDepth;
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
            if (isStatic && (equalSpans(text, "length"_s.span8()) || equalSpans(text, "name"_s.span8()) || equalSpans(text, "prototype"_s.span8())))
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

    // `C.name(..)`, `C.name`, `C.name = value`
    bool resolveOnClassObject(BlockID block, unsigned index)
    {
        Value original = m_function.at(m_function.at(block).values[index]);
        ValueID classObject = original.child(0);
        LayoutID ofClass = classDefinedBy(classObject);
        if (!ofClass || m_asksClassObject[block.index()] == ofClass)
            return false;
        auto member = memberFoundFrom(ofClass, original.string(), true);
        if (!member)
            return false;
        switch (original.opcode) {
        case Opcode::GetById:
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
        bool isGet = original.string() == m_superGet;
        if ((!isGet && original.string() != m_superSet) || original.arguments.size() != (isGet ? 4u : 5u))
            return false;
        LayoutID around = classDefinedBy(original.arguments[0]);
        auto& key = m_function.at(originOf(original.arguments[1]));
        auto& isStatic = m_function.at(originOf(original.arguments.last()));
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
            predicted = field.onlyFunction;
            break;
        }
        case Opcode::GetById:
            if (auto access = fieldOf(m_program, typeOf(definition.child(0)), definition.string()))
                predicted = m_program.at(access->layout).fields[access->field].onlyFunction;
            else if (definition.predictedByKey)
                predicted = m_program.functionByKey(definition.predictedByKey, definition.string());
            break;
        case Opcode::GetByVal:
            if (auto& key = m_function.at(originOf(definition.child(1))); definition.predictedByKey && key.opcode == Opcode::String)
                predicted = m_program.functionByKey(definition.predictedByKey, key.string());
            break;
        default:
            break;
        }
        // What is expected of the value itself comes from closer by than what is expected of the place.
        if (FunctionID ofValue = m_function.at(operand).predictedClosure ? m_function.at(operand).predictedClosure : definition.predictedClosure)
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
        for (auto& block : m_function.blocks) {
            if (block.terminator.kind != TerminatorKind::Branch)
                continue;
            auto& condition = m_function.at(block.terminator.value);
            if (condition.opcode == Opcode::IsClosureOf)
                m_refuted[block.terminator.targets[1].block.index()] = Refuted { originOf(condition.child(0)), condition.function() };
            if (condition.opcode == Opcode::IsClassObjectAsDeclared)
                m_asksClassObject[block.terminator.targets[1].block.index()] = condition.layout();
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
        if (!known || !mayBeCalledDirectly(known->function) || isRefuted(block, *known))
            return false;
        if (!m_mayGuess && !known->isCertain && !known->isCertainUnlessNil)
            return false;
        ValueID id = m_function.at(block).values[index];
        open(block, index);
        BlockID otherwise;
        if (!known->isCertain) {
            // `f(x) as T`: what the function yields is of a type that is known, and as a rule held already. What the general call yields is not.
            takeCastOfResult(id);
            otherwise = testClosure(original.child(0), *known);
        }
        callAndFinish(known->function, capturesOf(*known, original.child(0)).span(), original.child(1), original.arguments.span(), original.type, Yields::Result);
        if (otherwise) {
            moveTo(otherwise);
            // What is expected to be the function may be the function bound.
            if (FunctionID bound = known->isCertainUnlessNil ? FunctionID() : m_program.boundFunction(known->function, 0); bound && m_program.at(bound).kind == FunctionKind::Bound) {
                KnownCallee second { bound, known->definition, { }, false };
                BlockID neither = testClosure(original.child(0), second);
                callAndFinish(bound, capturesOf(second, original.child(0)).span(), original.child(1), original.arguments.span(), original.type, Yields::Result);
                moveTo(neither);
            }
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
        BuiltinID entry = value.opcode == Opcode::Operation ? Builtins::singleton().find(BuiltinKind::Operation, { }, m_program.builtinName(value.string())) : value.builtin();
        if (!entry || entry != m_callNoInline || value.arguments.size() < 2)
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
        static const BuiltinName name = Builtins::singleton().nameOf("bind"_s.span8());
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
        if (callee.captureTypes.size() + 1 + numberOfBoundArguments > UINT8_MAX)
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
            direct.immediates[0] = m_apply.index();
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
    bool mayConstructDirectly(FunctionID id) const
    {
        auto& callee = m_program.at(id);
        if (!mayBeCalledDirectly(id) || callee.kind != FunctionKind::Plain || !callee.captureTypes.isEmpty() || callee.hasManyClosures || !callee.has(Format::FunctionHasThis) || callee.has(Format::FunctionHasNewTarget))
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
        if (callee.has(Format::FunctionIsAsync | Format::FunctionIsGenerator) || callee.resumeFunction || callee.thisType)
            return false;
        // It reads `new.target` from where only the run time puts it.
        if (callee.kind == FunctionKind::Plain)
            return !callee.has(Format::FunctionHasNewTarget) && mayBeCalledDirectly(id);
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
        if (isMadeBySuperclass)
            made = returned;
        else if (mayReturnObject(callee)) {
            Value test = make(Opcode::CallBuiltin, m_program.types.boolean);
            test.immediates[0] = m_isObject.index();
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
        return parent && m_isObject && constructsOrdinaryObject(parent) && m_program.at(parent).captureTypes.isEmpty();
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
        if (!parent || !m_isObject || !constructsOrdinaryObject(parent) || !m_program.at(parent).captureTypes.isEmpty())
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
        if (isMadeBySuperclass)
            made = returned;
        else if (mayReturnObject(callee)) {
            Value test = make(Opcode::CallBuiltin, m_program.types.boolean);
            test.immediates[0] = m_isObject.index();
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
        if (original.string() != m_constructWith || original.arguments.size() < 2)
            return false;
        auto known = knownCallee(original.arguments[0]);
        if (!known || !mayConstructDirectly(known->function) || isRefuted(block, *known))
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
        if (!known->isCertain) {
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
            ValueID general = appendThrowing(WTF::move(original));
            if (known->isCertainUnlessNil)
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
        } else if (m_isObject && constructsOrdinaryObject(known->function))
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
    StringID m_constructWith;
    StringID m_superGet;
    StringID m_superSet;
    // The operation: a call with the arguments in a list.
    BuiltinID m_apply;
    BuiltinID m_isObject;
    BuiltinID m_callNoInline;
    // By block: the value, by its definition, that is known there not to be a closure of the function.
    Vector<Refuted> m_refuted;
    // By block: the class, where it is the arm of askClassObjectUnlessAsDeclared() that asks.
    Vector<LayoutID> m_asksClassObject;

    // Of the value that is being replaced.
    uint32_t m_position { 0 };
    uint32_t m_inlineFrame { 0 };
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

unsigned resolveCalls(const Program& program, Function& function, bool mayGuess)
{
    return CallResolution(program, function, mayGuess).run();
}

// The way in for callers that do not know the function: it takes the function value, `this` and the arguments as the engine passes
// them, holds them to the types of the parameters, and calls the function. That of a constructor is only ever entered by the run time,
// which has made `this` and passes `new.target` for the first argument if the constructor takes it: see State::createThis().
static Function createGenericEntrypoint(const Program& program, FunctionID id)
{
    auto& callee = program.at(id);
    auto& types = program.types;
    Function entry;
    entry.name = callee.name;
    entry.kind = callee.kind;
    entry.constructedClass = callee.constructedClass;
    entry.declaredAs = callee.declaredAs;
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
    auto append = [&](Value&& value) {
        ValueID result(entry.values.size());
        entry.values.append(WTF::move(value));
        entry.blocks[0].values.append(result);
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
        cast.assertedType = asserted;
        cast.children[0] = value;
        cast.immediates[1] = static_cast<uint32_t>(reason);
        return append(WTF::move(cast));
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
    call.immediates[0] = id.index();
    call.arguments = WTF::move(all);
    entry.blocks[0].terminator.kind = TerminatorKind::Return;
    entry.blocks[0].terminator.value = append(WTF::move(call));
    return entry;
}

void findDeclaredParents(Program& program)
{
    auto makeClass = program.find("MakeClass"_s.span8());
    if (!makeClass)
        return;
    for (auto& function : program.functions) {
        auto constantClosure = [&](ValueID id) {
            while (function.at(id).opcode == Opcode::Cast)
                id = function.at(id).child(0);
            return function.at(id).opcode == Opcode::FunctionReference ? function.at(id).function() : FunctionID();
        };
        for (auto& value : function.values) {
            if (value.opcode != Opcode::Operation || value.string() != *makeClass || value.arguments.size() != 2)
                continue;
            FunctionID constructor = constantClosure(value.arguments[0]);
            FunctionID parent = constantClosure(value.arguments[1]);
            if (!constructor || !parent || program.at(parent).kind != FunctionKind::Constructor)
                continue;
            if (LayoutID ofClass = program.at(constructor).constructedClass; ofClass && program.at(ofClass).isEvaluatedOnce && !program.at(ofClass).base)
                program.at(ofClass).declaredParentConstructor = parent;
        }
    }
}

void createGenericEntrypoints(Program& program)
{
    for (unsigned i = 0; i < program.layouts.size(); ++i) {
        if (!program.layouts[i].type)
            program.layouts[i].type = program.types.intern(Type::view(LayoutID(i)));
    }
    for (unsigned i = 0; i < program.functions.size(); ++i) {
        for (auto& value : program.functions[i].values) {
            if (value.opcode == Opcode::NewClosure || value.opcode == Opcode::NewClosureOnce)
                program.at(value.function()).hasManyClosures = true;
        }
    }
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
    for (unsigned i = 0; i < program.functions.size(); ++i) {
        if (program.functions[i].has(Format::FunctionIsUsedAsValue))
            isValue.set(i);
    }
    for (auto& layout : program.layouts) {
        if (layout.constructor)
            isValue.set(layout.constructor.index());
        for (auto* methods : { &layout.methods, &layout.staticMethods }) {
            for (auto& method : *methods)
                isValue.set(method.function.index());
        }
    }
    for (unsigned i : isValue) {
        if (program.functions[i].isGenericEntrypoint() || program.genericEntrypoints.contains(i))
            continue;
        Function entry = createGenericEntrypoint(program, FunctionID(i));
        program.genericEntrypoints.add(i, FunctionID(program.functions.size()));
        program.functions.append(WTF::move(entry));
    }
}

// It takes what the function captures, `this` where the function takes one, the arguments that are bound, and the parameters that are left, each as the function declares it.
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

void createBoundFunctions(Program& program)
{
    Vector<uint64_t> wanted;
    for (auto& function : program.functions) {
        if (!function.isGenericEntrypoint())
            CallResolution(program, function).collectBoundFunctions(wanted);
    }
    // In the order of the program, whatever the table makes of them.
    for (uint64_t key : wanted) {
        if (program.boundFunctions.contains(key))
            continue;
        FunctionID function(static_cast<uint32_t>(key >> 2));
        Vector<uint8_t> name;
        name.append("bound "_s.span8());
        name.append(program.strings[program.at(function).name.index()]);
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
