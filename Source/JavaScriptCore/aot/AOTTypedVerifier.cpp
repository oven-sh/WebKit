/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedPasses.h"

#if ENABLE(AOT)

#include <wtf/StringPrintStream.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

// Compiled code tests nothing that a type says. So a value that is used as something its type does not say would be read as what it is
// not. This makes sure there is none: it goes once over a function and compares types that are given. It finds out no type.
class Verifier {
public:
    Verifier(const Program& program, FunctionID id, const Function& function, Vector<String>& findings)
        : m_program(program)
        , m_id(id)
        , m_function(function)
        , m_findings(findings)
        , m_newObjectOfClass(program.find("NewObjectOfClass"_s.span8()).value_or(StringID()))
    {
    }

    void run()
    {
        if (m_function.blocks.isEmpty())
            return;
        verifyEntry();
        // What nothing gets to is never run, and not lowered.
        BitVector isReached;
        Vector<unsigned> worklist { 0u };
        isReached.set(0);
        while (!worklist.isEmpty()) {
            for (auto& target : m_function.blocks[worklist.takeLast()].terminator.targets) {
                if (!isReached.set(target.block.index()))
                    worklist.append(target.block.index());
            }
        }
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            if (!isReached.get(b))
                continue;
            auto& block = m_function.blocks[b];
            for (ValueID id : block.values)
                verify(id, m_function.at(id));
            verify(block.terminator);
        }
        for (unsigned v = 0; v < m_function.values.size(); ++v) {
            auto& value = m_function.values[v];
            if (!value.type)
                fail(ValueID(v), "has no type"_s);
            else if (value.rep != repOf(m_program.at(value.type)))
                fail(ValueID(v), "is not kept the way its type says"_s);
        }
    }

private:
    const Type& typeOf(ValueID id) const { return m_program.at(m_function.at(id).type); }

    void fail(ValueID id, ASCIILiteral what)
    {
        StringPrintStream out;
        out.print("f", m_id.index(), " `", String::fromUTF8(m_program.string(m_function.name)), "`: v", id.index(), " ", nameOf(m_function.at(id).opcode), " ", what);
        m_findings.append(out.toString());
    }

    void fail(ValueID user, ValueID operand, const Type& needed, ASCIILiteral as)
    {
        StringPrintStream out;
        out.print("f", m_id.index(), " `", String::fromUTF8(m_program.string(m_function.name)), "`: v", operand.index(), " of type ");
        m_program.dump(out, typeOf(operand));
        out.print(" is used as ", as, " of type ");
        m_program.dump(out, needed);
        if (user)
            out.print(" by v", user.index(), " ", nameOf(m_function.at(user).opcode));
        m_findings.append(out.toString());
    }

    void need(ValueID user, ValueID operand, const Type& needed, ASCIILiteral as)
    {
        if (!typeOf(operand).isWithin(needed, m_program))
            fail(user, operand, needed, as);
    }

    // None: anything will do.
    void need(ValueID user, ValueID operand, TypeID needed, ASCIILiteral as)
    {
        if (needed)
            need(user, operand, m_program.at(needed), as);
    }

    // The prototype of an object goes with its allocation, and whether an object is of a class goes by its shape.
    void needOneClassObject(ValueID id, LayoutID layout)
    {
        if (m_program.at(layout).kind == LayoutKind::Instance && !(m_program.at(layout).constructor && m_program.at(layout).isLaidOut && m_program.at(layout).isEvaluatedOnce))
            fail(id, "names a class that is not laid out or not evaluated once"_s);
    }

    // GetField and PutField go by where the field is, untested. The type of the operand vouches for that, but for a layout that admits
    // foreign objects, of which any object is. There the operand has to be an allocation of this function.
    void needObjectWithFieldsOf(ValueID id, const Value& value)
    {
        need(id, value.child(0), Type::view(value.layout()), "an object"_s);
        auto& layout = m_program.at(value.layout());
        if (!layout.admitsForeignObjects)
            return;
        ValueID made = value.child(0);
        while (m_function.at(made).opcode == Opcode::Cast)
            made = m_function.at(made).child(0);
        if (m_function.at(made).opcode != Opcode::NewObjectWithShape || (m_function.at(made).layout() != value.layout() && !std::ranges::binary_search(layout.members, m_function.at(made).layout())))
            fail(id, "names a field of what may be no object of the layout"_s);
    }

    void verifyEntry()
    {
        auto& entry = m_function.blocks[0].parameters;
        unsigned at = 0;
        auto is = [&](const Type& type, ASCIILiteral what) {
            if (at >= entry.size())
                return;
            // Whoever calls passes what the declaration says. The body may take it for no less.
            if (!type.isWithin(typeOf(entry[at]), m_program))
                fail(ValueID(), entry[at], type, what);
            ++at;
        };
        if (m_function.environment)
            is(Type::view(m_function.environment), "the environment"_s);
        if (m_function.has(FunctionUsesArgumentBuffer) || m_function.isGenericEntrypoint())
            return;
        if (m_function.has(Format::FunctionHasThis))
            is(m_function.thisType ? m_program.at(m_function.thisType) : Type::anything(), "`this`"_s);
        for (TypeID parameter : m_function.parameterTypes)
            is(parameter ? m_program.at(parameter) : Type::anything(), "a parameter"_s);
    }

    void verify(ValueID id, const Value& value)
    {
        switch (value.opcode) {
        case Opcode::PutModuleVariable:
            need(id, value.child(0), m_program.variables[value.variable().index()].type, "what the variable holds"_s);
            break;
        case Opcode::NewObjectWithShape:
            if (m_program.at(value.type) != Type::view(value.layout()))
                fail(id, "is not of the layout it is made with"_s);
            needOneClassObject(id, value.layout());
            break;
        case Opcode::IsInstanceOfClass:
            if (m_program.at(value.layout()).kind != LayoutKind::Instance)
                fail(id, "asks for the objects of what is no class"_s);
            needOneClassObject(id, value.layout());
            break;
        // The closure of a constructor is the class object.
        case Opcode::FunctionReference:
            if (auto& function = m_program.at(value.function()); function.kind == FunctionKind::Constructor && !(function.constructedClass && m_program.at(function.constructedClass).isEvaluatedOnce))
                fail(id, "is the one class object of a class that is not evaluated once"_s);
            break;
        case Opcode::GetField:
            needObjectWithFieldsOf(id, value);
            break;
        case Opcode::PutField:
            needObjectWithFieldsOf(id, value);
            need(id, value.child(1), m_program.at(value.layout()).fields[value.field()].type, "what the field holds"_s);
            break;
        case Opcode::PutById:
            if (auto access = fieldOf(m_program, typeOf(value.child(0)), value.string()))
                need(id, value.child(1), m_program.at(access->layout).fields[access->field].type, "what the field holds"_s);
            break;
        case Opcode::NewClosure:
            if (LayoutID environment = m_program.at(value.function()).environment)
                need(id, value.child(0), Type::view(environment), "the environment"_s);
            if (m_program.at(value.function()).kind == FunctionKind::Constructor) {
                LayoutID ofClass = m_program.at(value.function()).constructedClass;
                if (LayoutID environment = ofClass ? m_program.at(ofClass).environment : LayoutID())
                    need(id, value.child(0), Type::view(environment), "the environment of a class"_s);
                else
                    fail(id, "is another class object of a class that is evaluated once"_s);
            }
            break;
        case Opcode::CallDirect: {
            auto& callee = m_program.at(value.function());
            if (callee.has(FunctionUsesArgumentBuffer) || callee.isGenericEntrypoint())
                break;
            unsigned at = 0;
            auto pass = [&](const Type& type, ASCIILiteral as) {
                if (at < value.arguments.size())
                    need(id, value.arguments[at], type, as);
                ++at;
            };
            if (callee.environment)
                pass(Type::view(callee.environment), "the environment"_s);
            if (callee.has(Format::FunctionHasThis))
                pass(callee.thisType ? m_program.at(callee.thisType) : Type::anything(), "`this`"_s);
            for (TypeID parameter : callee.parameterTypes)
                pass(parameter ? m_program.at(parameter) : Type::anything(), "an argument"_s);
            break;
        }
        case Opcode::Cast:
            if (value.castReason() == CastReason::Upcast)
                need(id, value.child(0), value.type, "a value"_s);
            // Whatever comes of it is of the type of the cast untested: a value of the type after `as`, or the object that went in.
            if (value.castReason() == CastReason::Coerce) {
                auto& type = m_program.at(value.type);
                auto& coercedTo = m_program.at(value.coercedType());
                if (!coercedTo.isWithin(type, m_program))
                    fail(id, "coerces to a type that its own does not take in"_s);
                if (coercedTo.hasObjects() && !typeOf(value.child(0)).withoutPrimitives(Primitive::All).isWithin(type, m_program))
                    fail(id, value.child(0), type, "an object that a coercion may leave as it is"_s);
            }
            break;
        // The run time makes the object, so no allocation says that the layout has objects.
        case Opcode::Operation:
            if (value.string() == m_newObjectOfClass) {
                LayoutID made = m_program.at(value.type).onlyView();
                if (!made || m_program.at(made).kind != LayoutKind::Instance || !m_program.at(made).isAllocated)
                    fail(id, "makes an object of what is no class that has objects"_s);
            }
            break;
        default:
            break;
        }
    }

    void verify(const Terminator& terminator)
    {
        for (auto& target : terminator.targets) {
            auto& parameters = m_function.at(target.block).parameters;
            for (unsigned i = 0; i < target.arguments.size() && i < parameters.size(); ++i)
                need(parameters[i], target.arguments[i], m_function.at(parameters[i]).type, "a block parameter"_s);
        }
        switch (terminator.kind) {
        case TerminatorKind::Branch:
            need(ValueID(), terminator.value, m_program.types.boolean, "a condition"_s);
            break;
        case TerminatorKind::Switch:
            need(ValueID(), terminator.value, Type::integer(Range::full()), "what a switch goes by"_s);
            break;
        case TerminatorKind::Return:
            // What an async function or a generator is declared to yield is not what `return` carries in its body.
            if (terminator.value && !m_function.has(Format::FunctionIsAsync | Format::FunctionIsGenerator))
                need(ValueID(), terminator.value, m_function.resultType, "the result"_s);
            break;
        default:
            break;
        }
    }

    const Program& m_program;
    FunctionID m_id;
    const Function& m_function;
    Vector<String>& m_findings;
    StringID m_newObjectOfClass;
};

} // anonymous namespace

void verify(const Program& program, FunctionID id, const Function& function, Vector<String>& findings)
{
    Verifier(program, id, function, findings).run();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
