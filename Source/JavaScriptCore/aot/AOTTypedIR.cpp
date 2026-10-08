/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedIR.h"

#if ENABLE(AOT)

#include <wtf/TZoneMallocInlines.h>

namespace JSC { namespace AOT { namespace Typed {

WTF_MAKE_TZONE_ALLOCATED_IMPL(Program);

ASCIILiteral nameOf(Opcode opcode)
{
    switch (opcode) {
#define AOT_TYPED_OPCODE_NAME(name) \
    case Opcode::name: \
        return #name ""_s;
        FOR_EACH_AOT_TYPED_OPCODE(AOT_TYPED_OPCODE_NAME)
        FOR_EACH_AOT_TYPED_INTERNAL_OPCODE(AOT_TYPED_OPCODE_NAME)
#undef AOT_TYPED_OPCODE_NAME
    }
    RELEASE_ASSERT_NOT_REACHED();
}

namespace {

class Dumper {
public:
    Dumper(const Program& program, PrintStream& out)
        : m_program(program)
        , m_out(out)
    {
    }

    void run()
    {
        for (size_t i = 0; i < m_program.layouts.size(); ++i)
            dumpLayout(i);
        for (size_t i = 0; i < m_program.variables.size(); ++i) {
            auto& variable = m_program.variables[i];
            m_out.print("variable ", i, " ");
            quoted(variable.name);
            m_out.print(" module ");
            index(variable.module.index());
            m_out.print(": ");
            type(variable.type);
            m_out.print(variable.startsUndefined ? " undefined" : "", variable.isNeverSeenUninitialized ? " never seen uninitialized" : "", variable.isStoredOnce ? ", stored once" : "", "\n");
        }
        for (size_t i = 0; i < m_program.modules.size(); ++i) {
            m_out.print("module ", i, " ");
            quoted(m_program.modules[i].path);
            m_out.print(" f", m_program.modules[i].initializer.index(), "\n");
        }
        m_out.print("entry f", m_program.entry.index(), "\n");
        for (size_t i = 0; i < m_program.functions.size(); ++i)
            dumpFunction(i);
    }

    void run(FunctionID function)
    {
        dumpFunction(function.index());
    }

private:
    void index(uint32_t value)
    {
        if (value == Format::none)
            m_out.print("-");
        else
            m_out.print(value);
    }

    void type(TypeID id)
    {
        if (!id)
            m_out.print("-");
        else
            m_program.dump(m_out, m_program.at(id));
    }

    // The type of a value, and after a tilde what is expected of it.
    void typeAndPrediction(const Value& value)
    {
        type(value.type);
        if (value.prediction) {
            m_out.print(" ~");
            type(value.prediction);
        }
        if (value.predictedClosure)
            m_out.print(" ~f", value.predictedClosure.index());
    }

    void quoted(StringID id)
    {
        if (!id) {
            m_out.print("-");
            return;
        }
        m_out.print("\"");
        for (uint8_t byte : m_program.string(id)) {
            if (byte < 0x20 || byte >= 0x7f || byte == '"' || byte == '\\')
                m_out.printf("\\x%02x", byte);
            else
                m_out.printf("%c", byte);
        }
        m_out.print("\"");
    }

    void dumpLayout(size_t i)
    {
        auto& layout = m_program.layouts[i];
        static constexpr ASCIILiteral kinds[] = { "Record"_s, "Instance"_s, "Environment"_s, "Closure"_s };
        m_out.print("shape ", i, " ", kinds[static_cast<unsigned>(layout.kind)], " ");
        quoted(layout.name);
        m_out.print(" base ");
        index(layout.base.index());
        if (layout.kind == LayoutKind::Instance) {
            m_out.print(" constructor ");
            index(layout.constructor.index());
            m_out.print(" environment ");
            index(layout.environment.index());
            m_out.print(layout.isLaidOut ? " laid out" : "", layout.isEvaluatedOnce ? " evaluated once" : "", layout.hasForeignRoot ? " foreign root" : "", layout.hasFixedMembers ? " fixed members" : "", layout.mayHaveOtherPrototype ? " other prototypes" : "", layout.hasStaticMemberOfUnknownName ? " unknown static names" : "");
        }
        m_out.print(layout.isAllocated ? " allocated" : "", layout.mayBeFrozen ? " frozen" : "", layout.admitsForeignObjects ? " foreign" : "", "\n");
        auto layouts = [&](ASCIILiteral what, const Vector<LayoutID>& list) {
            if (list.isEmpty())
                return;
            m_out.print("  ", what);
            for (LayoutID other : list)
                m_out.print(" ", other.index());
            m_out.print("\n");
        };
        layouts("members"_s, layout.members);
        layouts("specializations"_s, layout.specializations);
        for (auto& field : layout.fields) {
            m_out.print("  field ");
            quoted(field.name);
            m_out.print(" ");
            type(field.type);
            // As the front end numbers them.
            m_out.print(" flags ", (field.flags & (fieldIsOptional | fieldIsImmutable)) | (field.flags & fieldIsMethod ? Format::FieldIsMethod : 0) | (field.flags & fieldIsAnnotated ? Format::FieldIsAnnotated : 0) | (field.flags & fieldIsNeverSeenUninitialized ? Format::FieldIsNeverSeenUninitialized : 0) | (field.flags & fieldIsStoredOnce ? Format::FieldIsStoredOnce : 0));
            for (auto& literal : field.literals) {
                if (literal.string) {
                    m_out.print(" ");
                    quoted(literal.string);
                } else
                    m_out.printf(" 0x%016llx", static_cast<unsigned long long>(std::bit_cast<uint64_t>(literal.number)));
            }
            m_out.print("\n");
        }
        static constexpr ASCIILiteral methodKinds[] = { "Method"_s, "Getter"_s, "Setter"_s };
        auto methods = [&](ASCIILiteral what, const Vector<Method>& list) {
            for (auto& method : list) {
                m_out.print("  ", what, " ");
                if (method.name)
                    quoted(method.name);
                else
                    m_out.print("[variable ", method.key, "]");
                m_out.print(" ", methodKinds[static_cast<unsigned>(method.kind)], " f", method.function.index(), method.mayBeReplaced ? " replaced\n" : "\n");
            }
        };
        methods("method"_s, layout.methods);
        methods("static method"_s, layout.staticMethods);
        for (auto& field : layout.staticFields) {
            m_out.print("  static field ");
            quoted(field.name);
            m_out.print(" variable ", field.variable, field.isAnnotated ? " annotated\n" : "\n");
        }
    }

    void list(const Arguments& arguments)
    {
        m_out.print("(");
        CommaPrinter comma(", "_s);
        for (ValueID argument : arguments)
            m_out.print(comma, "v", argument.index());
        m_out.print(")");
    }

    void target(const Target& target)
    {
        m_out.print("b", target.block.index());
        list(target.arguments);
    }

    void dumpValue(ValueID id, const Value& value)
    {
        m_out.print("    v", id.index(), ": ");
        typeAndPrediction(value);
        m_out.print(" = ", nameOf(value.opcode));
        CommaPrinter comma(", "_s, " "_s);
        for (ValueID child : value.children) {
            if (child)
                m_out.print(comma, "v", child.index());
        }
        switch (value.opcode) {
        case Opcode::Boolean:
            m_out.print(value.boolean() ? " true" : " false");
            break;
        case Opcode::Integer:
            m_out.print(" ", value.integer());
            break;
        case Opcode::Double:
            m_out.printf(" 0x%016llx", static_cast<unsigned long long>(value.bits()));
            break;
        case Opcode::GetExternalModule:
        case Opcode::String:
        case Opcode::BigInt:
        case Opcode::GetGlobal:
        case Opcode::GetGlobalForTypeOf:
        case Opcode::PutGlobal:
        case Opcode::GetById:
        case Opcode::PutById:
        case Opcode::InById:
        case Opcode::DeleteById:
        case Opcode::CallMethod:
        case Opcode::Operation:
            m_out.print(" ");
            quoted(value.string());
            break;
        case Opcode::FunctionReference:
        case Opcode::NewClosure:
        case Opcode::CallDirect:
        case Opcode::IsClosureOf:
            m_out.print(" f", value.function().index());
            break;
        case Opcode::GetModuleVariable:
        case Opcode::PutModuleVariable:
            m_out.print(" variable ", value.variable().index());
            break;
        case Opcode::NewObjectWithShape:
        case Opcode::IsInstanceOfClass:
        case Opcode::GetArgument:
        case Opcode::CreateRest:
            m_out.print(" ", value.immediates[0]);
            break;
        case Opcode::GetScope:
            m_out.print(" shape ", value.layout().index());
            break;
        case Opcode::GetField:
        case Opcode::PutField:
            m_out.print(" shape ", value.layout().index(), " field ", value.field());
            break;
        case Opcode::Cast: {
            static constexpr ASCIILiteral reasons[] = { "Store"_s, "As"_s, "NonNull"_s, "Claim"_s, "Flow"_s, "Upcast"_s, "Coerce"_s };
            m_out.print(" ", reasons[static_cast<unsigned>(value.castReason())]);
            if (value.castReason() == CastReason::Coerce) {
                m_out.print(" ");
                type(value.coercedType());
            }
            break;
        }
        case Opcode::CallBuiltin: {
            auto& builtin = Builtins::singleton().at(value.builtin());
            m_out.print(" ", builtin.holder, builtin.holder.isEmpty() ? "" : ".", builtin.name, "#", value.builtin().index());
            break;
        }
        default:
            break;
        }
        switch (value.opcode) {
        case Opcode::NewArray:
        case Opcode::Strcat:
        case Opcode::Call:
        case Opcode::Construct:
        case Opcode::CallMethod:
        case Opcode::CallDirect:
        case Opcode::CallBuiltin:
        case Opcode::Operation:
            m_out.print(" ");
            list(value.arguments);
            break;
        default:
            break;
        }
        m_out.print("\n");
    }

    void dumpTerminator(const Terminator& terminator)
    {
        m_out.print("    ");
        switch (terminator.kind) {
        case TerminatorKind::Jump:
            m_out.print("Jump ");
            target(terminator.targets[0]);
            break;
        case TerminatorKind::Branch:
            m_out.print("Branch v", terminator.value.index(), " ? ");
            target(terminator.targets[0]);
            m_out.print(" : ");
            target(terminator.targets[1]);
            break;
        case TerminatorKind::Switch:
            m_out.print("Switch v", terminator.value.index(), " [");
            for (size_t i = 0; i < terminator.cases.size(); ++i) {
                m_out.print(i ? ", " : "", terminator.cases[i], ": ");
                target(terminator.targets[i]);
            }
            m_out.print("] default ");
            target(terminator.targets.last());
            break;
        case TerminatorKind::Return:
            m_out.print("Return ");
            if (terminator.value)
                m_out.print("v", terminator.value.index());
            else
                m_out.print("-");
            break;
        case TerminatorKind::Throw:
            m_out.print("Throw v", terminator.value.index());
            if (!terminator.targets.isEmpty()) {
                m_out.print(" unwind ");
                target(terminator.targets[0]);
            }
            break;
        case TerminatorKind::Invoke:
            m_out.print("Invoke ");
            target(terminator.targets[0]);
            m_out.print(" unwind ");
            target(terminator.targets[1]);
            break;
        case TerminatorKind::Unreachable:
            m_out.print("Unreachable");
            break;
        }
        m_out.print("\n");
    }

    void dumpFunction(size_t i)
    {
        auto& function = m_program.functions[i];
        static constexpr ASCIILiteral kinds[] = { "Plain"_s, "Arrow"_s, "Method"_s, "Constructor"_s, "Module"_s };
        m_out.print("function ", i, " ");
        quoted(function.name);
        m_out.print(" ", kinds[static_cast<unsigned>(function.kind)], " flags ", function.flags, " module ");
        index(function.module.index());
        m_out.print(" environment ");
        index(function.environment.index());
        m_out.print(" this ");
        type(function.thisType);
        m_out.print(" (");
        CommaPrinter comma(", "_s);
        for (unsigned k = 0; k < function.parameterTypes.size(); ++k) {
            m_out.print(comma);
            type(function.parameterTypes[k]);
            if (function.annotatedParameters.get(k))
                m_out.print(" annotated");
        }
        m_out.print(") result ");
        type(function.resultType);
        m_out.print("\n");
        for (size_t b = 0; b < function.blocks.size(); ++b) {
            auto& block = function.blocks[b];
            m_out.print("  b", b, "(");
            CommaPrinter parameterComma(", "_s);
            for (ValueID parameter : block.parameters) {
                m_out.print(parameterComma, "v", parameter.index(), ": ");
                typeAndPrediction(function.at(parameter));
            }
            m_out.print("):\n");
            for (ValueID id : block.values)
                dumpValue(id, function.at(id));
            dumpTerminator(block.terminator);
        }
    }

    const Program& m_program;
    PrintStream& m_out;
};

} // anonymous namespace

void Program::dump(PrintStream& out) const
{
    Dumper(*this, out).run();
}

void Program::dump(PrintStream& out, FunctionID function) const
{
    Dumper(*this, out).run(function);
}

void Program::dump(PrintStream& out, const Type& type) const
{
    CommaPrinter bar("|"_s);
    auto text = [&](StringID id) {
        if (id)
            out.print(byteCast<char>(string(id)));
    };
    uint16_t primitives = type.primitives;
    if (primitives & Primitive::Undefined)
        out.print(bar, "undefined");
    if (primitives & Primitive::Null)
        out.print(bar, "null");
    if ((primitives & Primitive::Boolean) == Primitive::Boolean)
        out.print(bar, "boolean");
    else if (primitives & Primitive::True)
        out.print(bar, "true");
    else if (primitives & Primitive::False)
        out.print(bar, "false");
    if ((primitives & Primitive::Number) == Primitive::Number && type.integers == Range::full())
        out.print(bar, "number");
    else {
        if (primitives & Primitive::Int) {
            if (type.integers == Range::full())
                out.print(bar, "int");
            else if (type.integers == Range::int32())
                out.print(bar, "i32");
            else if (type.integers.least == type.integers.most)
                out.print(bar, type.integers.least);
            else
                out.print(bar, "int[", type.integers.least, ",", type.integers.most, "]");
        }
        if (primitives & Primitive::Double)
            out.print(bar, "double");
    }
    if (primitives & Primitive::String)
        out.print(bar, "string");
    if (primitives & Primitive::Symbol)
        out.print(bar, "symbol");
    if (primitives & Primitive::BigInt)
        out.print(bar, "bigint");
    if (primitives & Primitive::Absent)
        out.print(bar, "absent");
    if (type.anyObject)
        out.print(bar, "object");
    else if (type.anyFunction)
        out.print(bar, "function");
    for (LayoutID id : type.views) {
        out.print(bar);
        text(at(id).name);
        out.print(".", id.index());
    }
    for (uint16_t index : type.engineClasses) {
        out.print(bar, Builtins::singleton().classes()[index].text);
        if (type.elements && index == Builtins::singleton().arrayClass().index) {
            out.print("<");
            if (at(type.elements).views.isEmpty())
                dump(out, at(type.elements));
            else
                out.print("...");
            out.print(">");
        }
    }
    if (type.isNever())
        out.print("never");
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
