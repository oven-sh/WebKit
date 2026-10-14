/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedIR.h"

#if ENABLE(AOT)

#include "AOTTypedThreads.h"
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
            m_out.print(variable.startsUndefined ? " undefined" : "", variable.isNeverSeenUninitialized ? " never seen uninitialized" : "", variable.isStoredOnce ? ", stored once" : "", variable.startsZero ? " zero" : "");
            onlyFunction(variable.onlyFunction, variable.onlyFunctionIsFact);
            m_out.print("\n");
        }
        for (size_t i = 0; i < m_program.modules.size(); ++i) {
            m_out.print("module ", i, " ");
            quoted(m_program.modules[i].path);
            m_out.print(" f", m_program.modules[i].initializer.index(), " ");
            quoted(m_program.modules[i].displayPath);
            m_out.print(" line table ", m_program.modules[i].lineTableSize, " bytes\n");
        }
        for (auto& [id, function] : m_program.builtinModules)
            m_out.print("builtin module ", id, " f", function.index(), "\n");
        for (auto& [id, function, visibility] : m_program.builtinFunctions)
            m_out.print("builtin function ", id, " f", function.index(), " visibility ", visibility, "\n");
        for (auto& [property, key, function] : m_program.functionsByKey) {
            m_out.print("by key ");
            quoted(property);
            m_out.print(" ");
            quoted(key);
            m_out.print(" f", function.index(), "\n");
        }
        if (!m_program.prototypePropertyNames.isEmpty()) {
            // Sorted by their bytes, as the front end lists them.
            Vector<StringID> names;
            for (size_t index : m_program.prototypePropertyNames)
                names.append(StringID(static_cast<uint32_t>(index)));
            std::ranges::sort(names, [&](StringID a, StringID b) {
                return std::ranges::lexicographical_compare(m_program.string(a), m_program.string(b));
            });
            m_out.print("prototype property names");
            for (StringID name : names) {
                m_out.print(" ");
                quoted(name);
            }
            m_out.print("\n");
        }
        for (FunctionID entry : m_program.entries)
            m_out.print("entry f", entry.index(), "\n");
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
        if (value.predictedByKey) {
            m_out.print(" ~[");
            quoted(value.predictedByKey);
            m_out.print("]");
        }
        if (value.assertedType) {
            m_out.print(" asserted ");
            type(value.assertedType);
        }
        if (value.inlineFrame)
            m_out.print(" frame ", value.inlineFrame);
        if (value.isInTailPosition)
            m_out.print(" tail");
    }

    void onlyFunction(FunctionID function, bool isFact)
    {
        if (function)
            m_out.print(" only f", function.index(), isFact ? " fact" : "");
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
        if (layout.declaredPath) {
            m_out.print(" declared ");
            quoted(layout.declaredPath);
            m_out.print(":", layout.declaredLine);
        }
        if (layout.kind == LayoutKind::Instance) {
            m_out.print(" constructor ");
            index(layout.constructor.index());
            m_out.print(" environment ");
            index(layout.environment.index());
            m_out.print(layout.isLaidOut ? " laid out" : "", layout.isEvaluatedOnce ? " evaluated once" : "", layout.hasForeignRoot ? " foreign root" : "", layout.hasFixedMembers ? " fixed members" : "", layout.mayHaveOtherPrototype ? " other prototypes" : "", layout.hasStaticMemberOfUnknownName ? " unknown static names" : "");
        }
        m_out.print(layout.kind != LayoutKind::Instance && layout.mayHaveOtherPrototype ? " other prototypes" : "", layout.isAllocated ? " allocated" : "", layout.mayBeFrozen ? " frozen" : "", layout.admitsForeignObjects ? " foreign" : "", layout.mayGiveUp ? " gives-up" : "", "\n");
        auto layouts = [&](ASCIILiteral what, const Vector<LayoutID>& list) {
            if (list.isEmpty())
                return;
            m_out.print("  ", what);
            for (LayoutID other : list)
                m_out.print(" ", other.index());
            m_out.print("\n");
        };
        layouts("members"_s, layout.members);
        layouts("members by fit"_s, layout.membersByFit);
        layouts("specializations"_s, layout.specializations);
        for (auto& field : layout.fields) {
            m_out.print("  field ");
            quoted(field.name);
            m_out.print(" ");
            type(field.type);
            // As the front end numbers them.
            m_out.print(" flags ", (field.flags & (fieldIsOptional | fieldIsImmutable)) | (field.flags & fieldIsMethod ? Format::FieldIsMethod : 0) | (field.flags & fieldIsAnnotated ? Format::FieldIsAnnotated : 0) | (field.flags & fieldIsNeverSeenUninitialized ? Format::FieldIsNeverSeenUninitialized : 0) | (field.flags & fieldIsStoredOnce ? Format::FieldIsStoredOnce : 0) | (field.flags & fieldIsNeverStored ? Format::FieldIsNeverStored : 0) | (field.flags & fieldIsExpectedInt32 ? Format::FieldIsExpectedInt32 : 0) | (field.flags & fieldIsSetAtCreation ? Format::FieldIsSetAtCreation : 0) | (field.flags & fieldMayBeSetAtCreation ? Format::FieldMayBeSetAtCreation : 0) | (field.flags & fieldIsAbsentAtBirth ? Format::FieldIsAbsentAtBirth : 0) | (field.flags & fieldIsNeverSeenAbsent ? Format::FieldIsNeverSeenAbsent : 0));
            for (auto& literal : field.literals) {
                if (literal.string) {
                    m_out.print(" ");
                    quoted(literal.string);
                } else
                    m_out.printf(" 0x%016llx", static_cast<unsigned long long>(std::bit_cast<uint64_t>(literal.number)));
            }
            if (field.typeText) {
                m_out.print(" type ");
                quoted(field.typeText);
            }
            onlyFunction(field.onlyFunction, field.onlyFunctionIsFact);
            if (field.otherFunction)
                m_out.print(" or f", field.otherFunction.index(), " fact");
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
            m_out.print(" ");
            type(field.type);
            m_out.print("\n");
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
            if (value.opcode == Opcode::GetById && value.readsMethodFor())
                m_out.print(" unless original");
            if (value.opcode == Opcode::PutById && value.isInitialStore())
                m_out.print(" initial ", value.initialStoreIndex());
            if (value.isAtKnownPlace())
                m_out.print(" at place ", value.knownPlace(), value.receiverIsNeverNil() ? ", never nil" : "");
            if (value.isStoreToObjectBornWhole() && value.isAtKnownPlace())
                m_out.print(", born whole");
            else if (value.isStoreToObjectBornWhole())
                m_out.print(" born whole with place ", value.knownPlace(), ", goes by the name");
            if (value.isStoreToObjectBornWhole() && value.birthOfFirstStore())
                m_out.print(", row ", value.birthOfFirstStore() - 1);
            if (value.isFirstStoreToThis())
                m_out.print(value.isDefinitionInRowOfThis() ? " a definition" : "", " of the row of this, place ", value.knownPlace(), ", row ", static_cast<int>(value.birthOfFirstStore()) - 1);
            break;
        case Opcode::FunctionReference:
        case Opcode::NewClosure:
        case Opcode::CallDirect:
            m_out.print(" f", value.function().index());
            break;
        case Opcode::IsClosureOf:
            m_out.print(" f", value.function().index(), value.immediates[1] == closureIsAsMade ? " as made" : value.immediates[1] == closureEvenIfAltered ? " even if altered" : value.immediates[1] == closureIsTheOneThatConstructs ? " the one that constructs" : "");
            break;
        case Opcode::TypeOfIs:
            m_out.print(" ", value.typeofType());
            break;
        case Opcode::GetByVal:
            if (value.readsListedKey())
                m_out.print(" listed");
            if (value.readsForInList())
                m_out.print(" of the list");
            break;
        case Opcode::ResultAt:
            m_out.print(" ", value.resultIndex());
            break;
        case Opcode::IsBuiltinFunction:
        case Opcode::BuiltinFunction: {
            auto& builtin = Builtins::singleton().at(value.builtin());
            m_out.print(" ", builtin.holder, ".", builtin.name);
            break;
        }
        case Opcode::GetModuleVariable:
        case Opcode::PutModuleVariable:
            m_out.print(" variable ", value.variable().index(), value.findsVariableInitialized() ? ", initialized" : "");
            break;
        case Opcode::GetStaticField:
        case Opcode::PutStaticField:
            m_out.print(" ", value.immediates[0], " ");
            quoted(m_program.at(value.layout()).staticFields[value.immediates[1]].name);
            break;
        case Opcode::NewObjectWithShape:
        case Opcode::IsInstanceOfClass:
        case Opcode::GetArgument:
        case Opcode::CreateRest:
        case Opcode::GetRestLength:
        case Opcode::CallForwardVarargs:
        case Opcode::ConstructSuperForwardVarargs:
            m_out.print(" ", value.immediates[0]);
            break;
        case Opcode::GetMyArgumentByVal:
            m_out.print(" ", value.immediates[0], value.readsArgumentsObject() ? " of arguments" : "");
            break;
        case Opcode::NewClosureOnce:
            m_out.print(" field ", value.field(), " f", value.function().index());
            break;
        case Opcode::GetCapture:
            m_out.print(" f", value.function().index(), " ", value.captureIndex());
            break;
        case Opcode::GetField:
        case Opcode::PutField:
            m_out.print(" shape ", value.layout().index(), " field ", value.field());
            break;
        case Opcode::Cast: {
            static constexpr ASCIILiteral reasons[] = { "Store"_s, "As"_s, "NonNull"_s, "Claim"_s, "Flow"_s, "Upcast"_s, "Coerce"_s };
            m_out.print(" ", reasons[static_cast<unsigned>(value.castReason())]);
            if (value.castReason() == CastReason::Coerce) {
                m_out.print(value.freshness() == Format::Freshness::WholeGraph ? " fresh graph " : value.coercesFreshData() ? " fresh " : " ");
                type(value.coercedType());
            }
            if (value.castReason() == CastReason::NonNull && value.operandText()) {
                m_out.print(" ");
                quoted(value.operandText());
            }
            break;
        }
        case Opcode::CallBuiltin: {
            auto& builtin = Builtins::singleton().at(value.builtin());
            m_out.print(" ", builtin.holder, builtin.holder.isEmpty() ? "" : ".", builtin.name, "#", value.builtin().index());
            if (value.builtin() == Builtins::singleton().hasOriginalStructureOperation() && value.engineClass() != Format::none)
                m_out.print(" ", Builtins::singleton().classes()[value.engineClass()].text);
            break;
        }
        default:
            break;
        }
        switch (value.opcode) {
        case Opcode::NewArray:
        case Opcode::NewClosure:
        case Opcode::NewClosureOnce:
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
        if (value.methodAskedByStub != MethodAskedByStub::None)
            m_out.print(" asked by stub ", static_cast<unsigned>(value.methodAskedByStub));
        if (value.classExpectedOfReceiverPlusOne)
            m_out.print(" expects ", Builtins::singleton().classes()[value.classExpectedOfReceiverPlusOne - 1].text);
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
        static constexpr ASCIILiteral kinds[] = { "Plain"_s, "Arrow"_s, "Method"_s, "Constructor"_s, "Module"_s, "Bound"_s };
        m_out.print("function ", i, " ");
        quoted(function.name);
        m_out.print(" ", kinds[static_cast<unsigned>(function.kind)], " flags ", function.flags, " module ");
        index(function.module.index());
        m_out.print(" captures (");
        CommaPrinter captureComma(", "_s);
        for (unsigned k = 0; k < function.captureTypes.size(); ++k) {
            m_out.print(captureComma);
            type(function.captureTypes[k]);
            if (function.capturesOnlyCalled.get(k))
                m_out.print(" only called");
        }
        m_out.print(") this ");
        type(function.thisType);
        m_out.print(" (");
        CommaPrinter comma(", "_s);
        for (unsigned k = 0; k < function.parameterTypes.size(); ++k) {
            m_out.print(comma);
            type(function.parameterTypes[k]);
            if (function.annotatedParameters.get(k))
                m_out.print(" annotated");
            if (function.parametersOnlyCalled.get(k))
                m_out.print(" only called");
        }
        m_out.print(") result ");
        type(function.resultType);
        if (function.takesItsClosure)
            m_out.print(" takes its closure");
        if (function.isUsedAsObject)
            m_out.print(" used as an object");
        if (m_program.genericEntrypoints.contains(static_cast<uint32_t>(&function - m_program.functions.span().data())))
            m_out.print(m_program.makesSlimClosures(function) ? " slim closures" : " full closures");
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

static uint64_t hashOfRow(std::span<const StringID> names, unsigned inlineCapacity, bool hasNullPrototype)
{
    uint64_t hash = inlineCapacity << 1 | hasNullPrototype;
    for (StringID name : names) {
        hash = (hash + name.index() + 1) * 0x9e3779b97f4a7c15ull;
        hash ^= hash >> 32;
    }
    return hash >> 1 | 1;
}

uint32_t Program::numberOfRow(std::span<const StringID> names, unsigned inlineCapacity, bool hasNullPrototype) const
{
    auto found = numbersOfRowsByHash.find(hashOfRow(names, inlineCapacity, hasNullPrototype));
    if (found == numbersOfRowsByHash.end())
        return Format::none;
    for (uint32_t number : found->value) {
        auto& row = numberedRows[number];
        if (row.inlineCapacity == inlineCapacity && row.hasNullPrototype == hasNullPrototype && std::ranges::equal(row.names, names))
            return number;
    }
    return Format::none;
}

uint32_t Program::ensureNumberOfRow(std::span<const StringID> names, unsigned inlineCapacity, bool hasNullPrototype)
{
    if (uint32_t number = numberOfRow(names, inlineCapacity, hasNullPrototype); number != Format::none)
        return number;
    NumberedRow row;
    row.names.append(names);
    row.inlineCapacity = inlineCapacity;
    row.hasNullPrototype = hasNullPrototype;
    numbersOfRowsByHash.add(hashOfRow(names, inlineCapacity, hasNullPrototype), Vector<uint32_t, 1>()).iterator->value.append(numberedRows.size());
    numberedRows.append(WTF::move(row));
    return numberedRows.size() - 1;
}

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

void Functions::listValuesInOrder()
{
    unsigned count = m_functions.size();
    m_isListed.clear();
    m_firstInOrder.fill(0, count + 1);
    forEachIndexInParallel(count, [&](unsigned i) {
        uint32_t number = 0;
        for (auto& block : m_functions[i].blocks)
            number += block.values.size();
        m_firstInOrder[i + 1] = number;
    });
    for (unsigned i = 0; i < count; ++i)
        m_firstInOrder[i + 1] += m_firstInOrder[i];
    m_valuesInOrder.clear();
    m_valuesInOrder.growWithoutInitializing(m_firstInOrder[count]);
    auto ids = m_valuesInOrder.items<&ValueInOrder::id>();
    auto opcodes = m_valuesInOrder.items<&ValueInOrder::opcode>();
    forEachIndexInParallel(count, [&](unsigned i) {
        auto& function = m_functions[i];
        uint32_t next = m_firstInOrder[i];
        for (auto& block : function.blocks) {
            for (ValueID id : block.values) {
                ids[next] = id;
                opcodes[next++] = function.at(id).opcode;
            }
        }
    });
    m_isListed.fill(1, count);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
