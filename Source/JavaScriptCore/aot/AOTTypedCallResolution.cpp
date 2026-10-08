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
// What is called or constructed is a function value. Which function is known where the operand is defined as one:
// FunctionReference, NewClosure, the read of a module variable that is only ever stored to once, and then such a function. Nothing is
// followed any further than that.
//
//     IsClosureOf callee, function    whether the value is a closure of the function
//
// is only needed for a variable that holds `undefined` at first.
//
// The receiver is the class object of a class that is evaluated once, where it is defined as that: FunctionReference of the constructor.
// Or the search starts behind such a class: `super`. What the classes declare and the program is not seen to replace stays where it
// is, so it is looked up here.
class CallResolution {
public:
    CallResolution(const Program& program, Function& function)
        : m_program(program)
        , m_function(function)
        , m_constructWith(program.find("ConstructWith"_s.span8()).value_or(StringID()))
        , m_superGet(program.find("SuperGet"_s.span8()).value_or(StringID()))
        , m_superSet(program.find("SuperSet"_s.span8()).value_or(StringID()))
    {
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
        ValueID environment;
        bool isCertain { true };
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
            return resolveByShape(block, index, MethodKind::Method) || resolveOnClassObject(block, index);
        case Opcode::GetById:
            return resolveByShape(block, index, MethodKind::Getter) || resolveOnClassObject(block, index);
        case Opcode::PutById:
            return resolveByShape(block, index, MethodKind::Setter) || resolveOnClassObject(block, index);
        case Opcode::Call:
            return resolveCall(block, index) || resolveCallOfMethodRead(block, index);
        case Opcode::Operation:
            return resolveConstruct(block, index) || resolveSuper(block, index);
        default:
            return false;
        }
    }

    // What is added, and where.

    BlockID addBlock()
    {
        m_function.blocks.append(Block { });
        m_refuted.append(Refuted { });
        return BlockID(m_function.blocks.size() - 1);
    }

    Value make(Opcode opcode, TypeID type) const
    {
        Value value;
        value.opcode = opcode;
        value.position = m_position;
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
        bool isCall = m_function.at(id).opcode == Opcode::CallMethod || m_function.at(id).opcode == Opcode::Call;
        m_inliningDepth = isCall ? m_function.at(id).immediates[1] : Format::none;
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
        if (callee.blocks.isEmpty() || callee.isGenericEntrypoint())
            return false;
        // [[Call]] of a class throws.
        if (callee.kind == FunctionKind::Constructor || callee.kind == FunctionKind::Module)
            return false;
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
    Arguments passedTo(FunctionID id, ValueID environment, ValueID thisValue, std::span<const ValueID> given)
    {
        auto& callee = m_program.at(id);
        Arguments all;
        if (callee.environment)
            all.append(environment);
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
    void callAndFinish(FunctionID id, ValueID environment, ValueID thisValue, std::span<const ValueID> given, TypeID type, Yields yields)
    {
        ValueID result = call(id, passedTo(id, environment, thisValue, given));
        if (yields == Yields::Undefined)
            result = append(make(Opcode::Undefined, m_program.types.undefined));
        finish(held(result, type, CastReason::Claim));
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
                // The environment of a method is in its closure, which is on the prototype.
                if (!callee || layout.mayHaveOtherPrototype || !mayBeCalledDirectly(callee) || m_program.at(callee).environment || !isFoundFirst(member, callee))
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
    bool mayBeCalledWithoutEnvironment(FunctionID id) const { return mayBeCalledDirectly(id) && !m_program.at(id).environment; }

    // The value at the index reads the member, with that for `this`.
    bool readMember(BlockID block, unsigned index, const Member& member, ValueID thisValue, TypeID type)
    {
        if (member.getter) {
            if (!mayBeCalledWithoutEnvironment(member.getter))
                return false;
            open(block, index);
            callAndFinish(member.getter, { }, thisValue, { }, type, Yields::Result);
            return true;
        }
        if (member.method && (m_program.at(member.method).environment || !m_program.genericEntrypoints.contains(member.method.index())))
            return false;
        open(block, index);
        // An accessor without a getter reads `undefined`.
        Value read = member.method ? make(Opcode::FunctionReference, m_program.types.function) : make(Opcode::Undefined, m_program.types.undefined);
        if (member.method)
            read.immediates[0] = member.method.index();
        finish(held(append(WTF::move(read)), type, CastReason::Claim));
        return true;
    }

    // Only a setter takes a store. The language has an error for any other, which is left to the run time.
    bool storeToMember(BlockID block, unsigned index, const Member& member, ValueID thisValue, ValueID stored, TypeID type)
    {
        if (!member.setter || !mayBeCalledWithoutEnvironment(member.setter))
            return false;
        open(block, index);
        callAndFinish(member.setter, { }, thisValue, std::span { &stored, 1 }, type, Yields::Undefined);
        return true;
    }

    // `C.name(..)`, `C.name`, `C.name = value`
    bool resolveOnClassObject(BlockID block, unsigned index)
    {
        Value original = m_function.at(m_function.at(block).values[index]);
        ValueID classObject = original.child(0);
        LayoutID ofClass = classDefinedBy(classObject);
        if (!ofClass)
            return false;
        auto member = memberFoundFrom(ofClass, original.string(), true);
        if (!member)
            return false;
        switch (original.opcode) {
        case Opcode::GetById:
            return readMember(block, index, *member, classObject, original.type);
        case Opcode::PutById:
            return storeToMember(block, index, *member, classObject, original.child(1), original.type);
        default:
            break;
        }
        if (!member->method || !mayBeCalledWithoutEnvironment(member->method))
            return false;
        open(block, index);
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
        if (!around || key.opcode != Opcode::String || isStatic.opcode != Opcode::Boolean)
            return false;
        // What the search starts behind keeps its prototype.
        if (!m_program.at(around).isLaidOut || !m_program.at(around).hasFixedMembers)
            return false;
        auto member = memberFoundFrom(m_program.at(around).base, key.string(), isStatic.boolean());
        if (!member)
            return false;
        if (isGet)
            return readMember(block, index, *member, original.arguments[2], original.type);
        return storeToMember(block, index, *member, original.arguments[3], original.arguments[2], original.type);
    }

    // By the definition of the callee.

    std::optional<KnownCallee> knownCallee(ValueID value) const
    {
        value = originOf(value);
        auto& definition = m_function.at(value);
        switch (definition.opcode) {
        case Opcode::FunctionReference:
            return KnownCallee { definition.function(), value, { }, true };
        case Opcode::NewClosure:
            return KnownCallee { definition.function(), value, definition.child(0), true };
        case Opcode::GetModuleVariable: {
            auto& variable = m_program.variables[definition.variable().index()];
            if (!variable.onlyFunction)
                return std::nullopt;
            // A read of a variable that holds nothing yet throws.
            return KnownCallee { variable.onlyFunction, value, { }, !variable.startsUndefined };
        }
        default:
            return std::nullopt;
        }
    }

    // Where a test for a function has failed, the general call that is made there stays.
    void noteRefutedCallees()
    {
        m_refuted.fill(Refuted { }, m_function.blocks.size());
        for (auto& block : m_function.blocks) {
            if (block.terminator.kind != TerminatorKind::Branch)
                continue;
            auto& condition = m_function.at(block.terminator.value);
            if (condition.opcode == Opcode::IsClosureOf)
                m_refuted[block.terminator.targets[1].block.index()] = Refuted { originOf(condition.child(0)), condition.function() };
        }
    }

    bool isRefuted(BlockID block, const KnownCallee& known) const
    {
        return m_refuted[block.index()] == Refuted { known.definition, known.function };
    }

    // Goes on where the value is a closure of the function. Yields where to go on for where it is not.
    BlockID testClosure(ValueID callee, const KnownCallee& known)
    {
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
        if (!known || !mayBeCalledDirectly(known->function) || isRefuted(block, *known))
            return false;
        open(block, index);
        BlockID otherwise;
        if (!known->isCertain)
            otherwise = testClosure(original.child(0), *known);
        callAndFinish(known->function, known->environment, original.child(1), original.arguments.span(), original.type, Yields::Result);
        if (otherwise) {
            moveTo(otherwise);
            finish(appendThrowing(WTF::move(original)));
        }
        return true;
    }

    // `new` on it makes an object, calls it with that for `this`, and yields the object. The object has the `prototype` of the one
    // closure there is of the function for its prototype.
    bool mayConstructDirectly(FunctionID id) const
    {
        auto& callee = m_program.at(id);
        if (!mayBeCalledDirectly(id) || callee.kind != FunctionKind::Plain || callee.environment || !callee.has(Format::FunctionHasThis))
            return false;
        // An object that it returns would take the place of the one that was made.
        for (auto& block : callee.blocks) {
            if (block.terminator.kind == TerminatorKind::Return && block.terminator.value && m_program.typeOf(callee, block.terminator.value).hasObjects())
                return false;
        }
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
        if (!isWithin(object, m_program.at(known->function).thisType))
            return false;
        // Each way makes its own object, right before it hands it over. Nobody can tell when an object was made that nobody has seen.
        size_t birth = m_function.at(block).values.find(object);
        if (birth == notFound || birth >= index || !isUsedOnlyBy(object, id))
            return false;
        m_function.at(block).values.removeAt(birth);

        open(block, index - 1);
        BlockID otherwise;
        if (!known->isCertain)
            otherwise = testClosure(original.arguments[0], *known);
        Allocation allocation = m_function.allocations[m_function.at(object).allocation()];
        allocation.constructedBy = known->function;
        uint32_t number = m_function.allocationOf(WTF::move(allocation));
        ValueID made = otherwise ? addValue(Value(m_function.at(object))) : object;
        m_function.at(made).immediates[1] = number;
        Arguments arguments = passedTo(known->function, { }, made, original.arguments.span().subspan(2));
        place(made);
        call(known->function, WTF::move(arguments));
        finish(held(made, original.type, CastReason::Claim));
        if (otherwise) {
            moveTo(otherwise);
            place(object);
            finish(appendThrowing(WTF::move(original)));
        }
        return true;
    }

    const Program& m_program;
    Function& m_function;
    StringID m_constructWith;
    StringID m_superGet;
    StringID m_superSet;
    // By block: the value, by its definition, that is known there not to be a closure of the function.
    Vector<Refuted> m_refuted;

    // Of the value that is being replaced.
    uint32_t m_position { 0 };
    // See inlineCalls().
    uint32_t m_inliningDepth { Format::none };
    bool m_isWatched { false };
    Target m_handler;
    BlockID m_after;
    // Where values are added.
    BlockID m_block;
    bool m_lastMayThrow { false };
};

} // anonymous namespace

unsigned resolveCalls(const Program& program, Function& function)
{
    return CallResolution(program, function).run();
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
    entry.environment = program.closureLayout;
    // What takes the rest of the arguments has to see them all, and so has what takes more than there are registers for.
    bool usesArgumentBuffer = callee.has(Format::FunctionHasRestParameter) || callee.parameterTypes.size() > numberOfArgumentGPRs;
    if (usesArgumentBuffer)
        entry.flags |= FunctionUsesArgumentBuffer;
    entry.parameterTypes.fill(TypeID(), callee.parameterTypes.size());
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
    auto held = [&](ValueID value, TypeID type, CastReason reason) {
        if (!type || program.isHeldAlready(program.typeOf(entry, value), program.at(type), reason))
            return value;
        Value cast = make(Opcode::Cast, type);
        cast.children[0] = value;
        cast.immediates[1] = static_cast<uint32_t>(reason);
        return append(WTF::move(cast));
    };

    ValueID closure = addParameter(program.at(program.closureLayout).type);
    ValueID thisValue = addParameter(types.anything);
    Vector<ValueID> given;
    for (unsigned i = 0; i < callee.parameterTypes.size(); ++i) {
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
        passed.immediates[0] = i;
        given.append(append(WTF::move(passed)));
    }
    Arguments all;
    if (callee.environment) {
        Value environment = make(Opcode::GetScope, program.at(callee.environment).type);
        environment.children[0] = closure;
        environment.immediates[0] = callee.environment.index();
        all.append(append(WTF::move(environment)));
    }
    if (callee.has(Format::FunctionHasThis))
        all.append(held(thisValue, callee.thisType, CastReason::Store));
    for (unsigned i = 0; i < given.size(); ++i)
        all.append(held(given[i], callee.parameterTypes[i], callee.reasonToHoldParameter(i)));
    Value call = make(Opcode::CallDirect, callee.resultType ? callee.resultType : types.anything);
    call.immediates[0] = id.index();
    call.arguments = WTF::move(all);
    entry.blocks[0].terminator.kind = TerminatorKind::Return;
    entry.blocks[0].terminator.value = append(WTF::move(call));
    return entry;
}

// Variable::onlyFunction.
static void findFunctionsInVariables(Program& program)
{
    Vector<unsigned> numberOfStores;
    numberOfStores.fill(0, program.variables.size());
    for (auto& function : program.functions) {
        for (auto& block : function.blocks) {
            for (ValueID id : block.values) {
                auto& store = function.at(id);
                if (store.opcode != Opcode::PutModuleVariable)
                    continue;
                ValueID stored = store.child(0);
                while (function.at(stored).opcode == Opcode::Cast)
                    stored = function.at(stored).child(0);
                bool isOnlyFunction = !numberOfStores[store.variable().index()]++ && function.at(stored).opcode == Opcode::FunctionReference;
                program.variables[store.variable().index()].onlyFunction = isOnlyFunction ? function.at(stored).function() : FunctionID();
            }
        }
    }
    // Loads of it are taken not to change.
    for (unsigned i = 0; i < program.variables.size(); ++i)
        RELEASE_ASSERT(!program.variables[i].isStoredOnce || numberOfStores[i] <= 1);
    // Whoever gets hold of the class object may store to these.
    for (auto& layout : program.layouts) {
        if (layout.environment)
            continue;
        for (auto& field : layout.staticFields)
            program.variables[field.variable].onlyFunction = { };
    }
}

void createGenericEntrypoints(Program& program)
{
    for (unsigned i = 0; i < program.layouts.size(); ++i) {
        if (!program.layouts[i].type)
            program.layouts[i].type = program.types.intern(Type::view(LayoutID(i)));
    }
    findFunctionsInVariables(program);
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

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
