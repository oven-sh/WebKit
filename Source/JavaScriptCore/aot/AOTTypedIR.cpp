/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedIR.h"

#if ENABLE(AOT)

#include "AOTTypedThreads.h"
#include <wtf/CommaPrinter.h>
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
            m_out.print(variable.startsUndefined ? " undefined" : "", variable.isNeverReadUninitialized ? " never read uninitialized" : "", variable.isStoredOnce ? ", stored once" : "", variable.startsZero ? " zero" : "");
            singleFunction(variable.singleFunction, variable.singleFunctionIsProven);
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

    void typeAndPrediction(const Value& value)
    {
        type(value.type);
        if (value.prediction()) {
            m_out.print(" ~");
            type(value.prediction());
        }
        if (value.predictedClosure())
            m_out.print(" ~f", value.predictedClosure().index());
        if (value.hasCallSignature())
            m_out.print(" #", value.callSignature());
        if (value.predictedByKey()) {
            m_out.print(" ~[");
            quoted(value.predictedByKey());
            m_out.print("]");
        }
        if (value.assertedType()) {
            m_out.print(" asserted ");
            type(value.assertedType());
        }
        if (value.inlineFrame)
            m_out.print(" frame ", value.inlineFrame);
        if (FunctionID callee = value.parameterCheckCallee())
            m_out.print(" parameter of f", callee.index());
        if (value.isInTailPosition)
            m_out.print(" tail");
        if (value.isAwaitedWithoutPromise)
            m_out.print(" without promise");
        if (value.takesElementsOfArrayLiteral)
            m_out.print(" takes elements");
        if (value.callsPredictedStatic)
            m_out.print(" calls predicted static");
        if (value.takesUnsharedString)
            m_out.print(" takes unshared string");
        if (value.tableInImage() != Format::none)
            m_out.print(" table in image ", value.tableInImage());
        if (value.dropsDroppableCallerFrame)
            m_out.print(" drops");
        if (value.leavesDroppableFrame)
            m_out.print(" droppable");
    }

    void singleFunction(FunctionID function, bool isProven)
    {
        if (function)
            m_out.print(" only f", function.index(), isProven ? " proven" : "");
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
            m_out.print(layout.hasFixedLayout ? " fixed-layout" : "", layout.isEvaluatedOnce ? " evaluated once" : "", layout.hasUntypedBase ? " untyped-base" : "", layout.hasImmutableMembers ? " fixed members" : "", layout.mayHaveOtherPrototype ? " other prototypes" : "", layout.hasNoOwnInstances ? " no own instances" : "", layout.hasComputedStaticMemberName ? " unknown static names" : "", layout.constructorMayBeReplaced ? " replaceable constructor" : "", layout.prototypeMayGainMembers ? " extensible prototype" : "");
        }
        m_out.print(layout.kind != LayoutKind::Instance && layout.mayHaveOtherPrototype ? " other prototypes" : "", layout.prototypeIsSetByTypedCode ? " prototype set by typed code" : "", layout.isAllocated ? " allocated" : "", layout.mayBeFrozen ? " frozen" : "", layout.acceptsUntypedObjects ? " accepts-untyped" : "", layout.isRelaxedForLogging ? " relaxed-for-logging" : "", layout.acceptsOnlyOtherLayouts ? " other-layouts-only" : "", layout.mayDespecialize ? " may-despecialize" : "", "\n");
        auto layouts = [&](ASCIILiteral what, const Vector<LayoutID>& list) {
            if (list.isEmpty())
                return;
            m_out.print("  ", what);
            for (LayoutID other : list)
                m_out.print(" ", other.index());
            m_out.print("\n");
        };
        layouts("subtypes"_s, layout.subtypes);
        layouts("structural subtypes"_s, layout.structuralSubtypes);
        layouts("specializations"_s, layout.specializations);
        for (auto& field : layout.fields) {
            m_out.print("  field ");
            quoted(field.name);
            m_out.print(" ");
            type(field.type);
            m_out.print(" flags ", (field.flags & (fieldIsOptional | fieldIsImmutable)) | (field.flags & fieldIsMethod ? Format::FieldIsMethod : 0) | (field.flags & fieldIsAnnotated ? Format::FieldIsAnnotated : 0) | (field.flags & fieldIsNeverReadUninitialized ? Format::FieldIsNeverReadUninitialized : 0) | (field.flags & fieldIsStoredOnce ? Format::FieldIsStoredOnce : 0) | (field.flags & fieldIsNeverStored ? Format::FieldIsNeverStored : 0) | (field.flags & fieldIsPredictedInt32 ? Format::FieldIsPredictedInt32 : 0) | (field.flags & fieldIsSetAtCreation ? Format::FieldIsSetAtCreation : 0) | (field.flags & fieldMayBeSetAtCreation ? Format::FieldMayBeSetAtCreation : 0) | (field.flags & fieldIsAbsentAtCreation ? Format::FieldIsAbsentAtCreation : 0) | (field.flags & fieldIsNeverObservedAbsent ? Format::FieldIsNeverObservedAbsent : 0) | (field.isUndeclared ? Format::FieldIsUndeclared : 0));
            for (auto& literal : field.literals) {
                if (literal.string) {
                    m_out.print(" ");
                    quoted(literal.string);
                } else
                    m_out.printf(" 0x%016llx", static_cast<unsigned long long>(std::bit_cast<uint64_t>(literal.number)));
            }
            if (field.typeSource) {
                m_out.print(" type ");
                quoted(field.typeSource);
            }
            singleFunction(field.singleFunction, field.singleFunctionIsProven);
            if (field.otherFunction)
                m_out.print(" or f", field.otherFunction.index(), " proven");
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
        case Opcode::ConstantSymbol:
        case Opcode::GetGlobal:
        case Opcode::GetGlobalForTypeOf:
        case Opcode::PutGlobal:
        case Opcode::GetById:
        case Opcode::PutById:
        case Opcode::InById:
        case Opcode::DeleteById:
        case Opcode::CallMethod:
            m_out.print(" ");
            quoted(value.string());
            if (value.isOnSlowPath)
                m_out.print(" on the slow path");
            if (value.isFusedWithMap)
                m_out.print(" fused with map");
            if (value.opcode == Opcode::GetById && value.methodGetByIdBuiltin())
                m_out.print(" unless original");
            if (value.opcode == Opcode::PutById && value.isInitialStore())
                m_out.print(" initial ", value.initialStoreIndex());
            if (value.isAtKnownSlot())
                m_out.print(" at slot ", value.knownSlot(), value.receiverIsNeverNullish() ? ", never nullish" : "");
            if (value.isStoreIntoInitialStructure() && value.isAtKnownSlot())
                m_out.print(", into initial structure");
            else if (value.isStoreIntoInitialStructure())
                m_out.print(" into initial structure at slot ", value.knownSlot(), ", by name");
            if (value.isStoreIntoInitialStructure() && value.initialStructureOfFirstStore())
                m_out.print(", structure ", value.initialStructureOfFirstStore() - 1);
            break;
        case Opcode::Operation:
            m_out.print(" \"", Builtins::singleton().at(value.builtin()).name, "\"");
            break;
        case Opcode::FunctionReference:
        case Opcode::NewClosure:
        case Opcode::CallDirect:
            m_out.print(" f", value.function().index());
            break;
        case Opcode::IsClosureOf:
            m_out.print(" f", value.function().index(), value.immediates[1] == closureIsUnmodified ? " unmodified" : value.immediates[1] == closureMayBeModified ? " possibly modified" : value.immediates[1] == closureIsConstructingCallee ? " constructing callee" : "");
            break;
        case Opcode::TypeOfIs:
            m_out.print(" ", value.typeofType());
            break;
        case Opcode::GetByVal:
            if (value.readsForInKey())
                m_out.print(" for-in key");
            if (value.readsForInKeys())
                m_out.print(" of for-in keys");
            break;
        case Opcode::ResultAt:
            m_out.print(" ", value.resultIndex());
            break;
        case Opcode::IsBuiltinFunction:
        case Opcode::BuiltinFunction: {
            if (BuiltinName global = value.comparedGlobal()) {
                m_out.print(" ", builtinNames[global.index()].text);
                break;
            }
            auto& builtin = Builtins::singleton().at(value.builtin());
            m_out.print(" ", builtin.holder, ".", builtin.name);
            break;
        }
        case Opcode::GetModuleVariable:
        case Opcode::PutModuleVariable:
            m_out.print(" variable ", value.variable().index(), value.readsInitializedVariable() ? ", initialized" : "");
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
        case Opcode::GetPlainRecordField:
            m_out.print(" shape ", value.layout().index(), " field ", value.field());
            break;
        case Opcode::IsPlainRecord:
            m_out.print(" shape ", value.layout().index(), value.immediates[1] == plainRecordIsConverted ? " converted" : "");
            break;
        case Opcode::Cast: {
            static constexpr ASCIILiteral reasons[] = { "Store"_s, "As"_s, "NonNull"_s, "Claim"_s, "Flow"_s, "Upcast"_s, "Coerce"_s, "CoerceFreshObject"_s, "CoerceFreshGraph"_s, "AsFreshObject"_s, "Member"_s };
            m_out.print(" ", reasons[static_cast<unsigned>(value.castReason())], value.keepsUnconvertibleObjects() ? " if convertible" : "");
            if (value.castReason() == CastReason::Coerce) {
                m_out.print(value.freshness() == Format::Freshness::WholeGraph ? " fresh graph " : value.coercesFreshData() ? " fresh " : " ");
                type(value.coercedType());
            }
            if (value.castReason() == CastReason::NonNull && value.operandSource()) {
                m_out.print(" ");
                quoted(value.operandSource());
            }
            break;
        }
        case Opcode::CallBuiltin: {
            auto& builtin = Builtins::singleton().at(value.builtin());
            m_out.print(" ", builtin.holder, builtin.holder.isEmpty() ? "" : ".", builtin.name, "#", value.builtin().index());
            if (value.builtin() == Builtins::singleton().originalStructureCheckOperation() && value.engineClass() != Format::none)
                m_out.print(" ", Builtins::singleton().classes()[value.engineClass()].text);
            if (value.receiverHasOriginalStructure)
                m_out.print(" original structure");
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
        if (value.speculatedMethod != SpeculatedMethod::None)
            m_out.print(" speculated method ", static_cast<unsigned>(value.speculatedMethod));
        if (value.predictedReceiverClassPlusOne)
            m_out.print(" expects ", Builtins::singleton().classes()[value.predictedReceiverClassPlusOne - 1].text);
        if (value.isStackAllocatedClosure())
            m_out.print(" in the frame");
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
            if (!terminator.cases.isEmpty())
                m_out.print(" at ", terminator.cases[0]);
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
            if (function.calledOnlyCaptures.get(k))
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
            if (function.calledOnlyParameters.get(k))
                m_out.print(" only called");
        }
        m_out.print(") result ");
        type(function.resultType);
        if (function.takesCalleeClosure)
            m_out.print(" takes closure");
        if (function.mayRunWithoutPromise)
            m_out.print(" may run without promise");
        if (function.isUsedAsObject)
            m_out.print(" used as an object");
        if (m_program.genericEntrypoints.contains(static_cast<uint32_t>(&function - m_program.functions.span().data())))
            m_out.print(m_program.usesSlimClosures(function) ? " slim closures" : " full closures");
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

static uint64_t hashOfStructureDescriptor(std::span<const StringID> names, unsigned inlineCapacity, bool hasNullPrototype)
{
    uint64_t hash = inlineCapacity << 1 | hasNullPrototype;
    for (StringID name : names) {
        hash = (hash + name.index() + 1) * 0x9e3779b97f4a7c15ull;
        hash ^= hash >> 32;
    }
    return hash >> 1 | 1;
}

uint32_t Program::findStructureDescriptor(std::span<const StringID> names, unsigned inlineCapacity, bool hasNullPrototype) const
{
    auto found = structureDescriptorsByHash.find(hashOfStructureDescriptor(names, inlineCapacity, hasNullPrototype));
    if (found == structureDescriptorsByHash.end())
        return Format::none;
    for (uint32_t number : found->value) {
        auto& row = structureDescriptors[number];
        if (row.inlineCapacity == inlineCapacity && row.hasNullPrototype == hasNullPrototype && std::ranges::equal(row.names, names))
            return number;
    }
    return Format::none;
}

uint32_t Program::ensureStructureDescriptor(std::span<const StringID> names, unsigned inlineCapacity, bool hasNullPrototype)
{
    if (uint32_t number = findStructureDescriptor(names, inlineCapacity, hasNullPrototype); number != Format::none)
        return number;
    StructureDescriptor row;
    row.names.append(names);
    row.inlineCapacity = inlineCapacity;
    row.hasNullPrototype = hasNullPrototype;
    structureDescriptorsByHash.add(hashOfStructureDescriptor(names, inlineCapacity, hasNullPrototype), Vector<uint32_t, 1>()).iterator->value.append(structureDescriptors.size());
    structureDescriptors.append(WTF::move(row));
    return structureDescriptors.size() - 1;
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
            else if (type.integers.minimum == type.integers.maximum)
                out.print(bar, type.integers.minimum);
            else
                out.print(bar, "int[", type.integers.minimum, ",", type.integers.maximum, "]");
        }
        if (primitives & Primitive::Double)
            out.print(bar, "double");
    }
    if (primitives & Primitive::String)
        out.print(bar, "string");
    if (primitives & Primitive::LiteralString)
        out.print(bar, "literal");
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
    for (LayoutID id : type.objectLayouts) {
        out.print(bar);
        text(at(id).name);
        out.print(".", id.index());
    }
    for (uint16_t index : type.builtinClasses) {
        out.print(bar, Builtins::singleton().classes()[index].text);
        if (type.elements && index == Builtins::singleton().arrayClass().index) {
            out.print("<");
            if (at(type.elements).objectLayouts.isEmpty())
                dump(out, at(type.elements));
            else
                out.print("...");
            out.print(">");
        }
    }
    if (type.isNever())
        out.print("never");
}

void Functions::rebuildInvalidatedValueIndex()
{
    size_t count = m_functions.size();
    if (m_isIndexed.size() == count && !m_hasInvalidatedFunctions)
        return;
    m_hasInvalidatedFunctions = 0;
    if (size_t indexedCount = m_isIndexed.size(); indexedCount < count) {
        m_isIndexed.grow(count);
        zeroSpan(m_isIndexed.mutableSpan().subspan(indexedCount));
        m_firstIndexedValue.grow(count);
        zeroSpan(m_firstIndexedValue.mutableSpan().subspan(indexedCount));
        m_indexedValueCount.grow(count);
        m_valueCount.grow(count);
        zeroSpan(m_valueCount.mutableSpan().subspan(indexedCount));
        m_contents.grow(count);
    }
    Vector<uint32_t> invalidated;
    auto isIndexed = m_isIndexed.span();
    for (size_t i = 0; i < count; ++i) {
        if (isIndexed[i]) {
            i = std::ranges::find(isIndexed.subspan(i), 0) - isIndexed.begin();
            if (i == count)
                break;
        }
        invalidated.append(i);
    }
    if (invalidated.isEmpty())
        return;
    uint32_t end = m_indexedValues.size();
    if (invalidated.size() == count) {
        end = 0;
        zeroSpan(m_firstIndexedValue.mutableSpan());
        zeroSpan(m_valueCount.mutableSpan());
        m_indexedValues.clear();
        m_valueOpcodes.shrink(0);
    }
    Vector<uint32_t> newValueCount(invalidated.size());
    forEachIndexInParallel(invalidated.size(), [&](unsigned k) {
        newValueCount[k] = m_functions[invalidated[k]].values.size();
    });
    for (size_t k = 0; k < invalidated.size(); ++k) {
        uint32_t i = invalidated[k];
        if (newValueCount[k] > m_valueCount[i]) {
            m_firstIndexedValue[i] = end;
            end += newValueCount[k];
        }
        m_valueCount[i] = newValueCount[k];
    }
    if (end > m_indexedValues.capacity()) {
        m_indexedValues.reserveCapacity(end + end / 4);
        m_valueOpcodes.reserveCapacity(end + end / 4);
    }
    m_indexedValues.growWithoutInitializing(end);
    m_valueOpcodes.grow(end);
    forEachIndexInParallel(invalidated.size(), [&](unsigned k) {
        indexValues(invalidated[k]);
    });
}

void Functions::releaseValueIndex()
{
    m_indexedValues.clearAndFree();
    m_valueOpcodes.clear();
    m_firstIndexedValue.clear();
    m_indexedValueCount.clear();
    m_valueCount.clear();
    m_contents.clear();
    m_isIndexed.clear();
}

void Functions::verifyValueIndex() const
{
    forEachIndexInParallel(m_functions.size(), [&](unsigned index) {
        if (!isIndexed(index))
            return;
        auto& function = m_functions[index];
        auto& contents = m_contents[index];
        RELEASE_ASSERT(m_valueCount[index] == function.values.size());
        auto valueOpcodes = m_valueOpcodes.span().subspan(m_firstIndexedValue[index], m_valueCount[index]);
        for (size_t k = 0; k < valueOpcodes.size(); ++k) {
            RELEASE_ASSERT(valueOpcodes[k] == function.values[k].opcode);
            RELEASE_ASSERT(contents.opcodes.contains(valueOpcodes[k]));
            RELEASE_ASSERT(contents.containsPredictedClosure || !function.values[k].predictedClosure());
        }
        auto opcodes = indexedOpcodes(index);
        auto ids = m_indexedValues.items<&IndexedValue::id>().subspan(m_firstIndexedValue[index], opcodes.size());
        size_t next = 0;
        for (auto& block : function.blocks) {
            for (ValueID id : block.values) {
                RELEASE_ASSERT(next < ids.size() && ids[next] == id && opcodes[next] == function.at(id).opcode);
                ++next;
            }
            RELEASE_ASSERT(contents.terminatorKinds >> static_cast<unsigned>(block.terminator.kind) & 1);
        }
        RELEASE_ASSERT(next == ids.size());
    });
}

bool Functions::fillValueIndexRows(const Function& function, std::span<ValueID> ids, std::span<Opcode> opcodes, std::span<Opcode> valueOpcodes, Contents& contents, uint32_t& indexedValueCount)
{
    size_t valueCount = function.values.size();
    for (size_t k = 0; k < valueCount; ++k) {
        auto& value = function.values[k];
        valueOpcodes[k] = value.opcode;
        contents.opcodes.add(value.opcode);
        contents.containsPredictedClosure |= !!value.predictedClosure();
    }
    size_t next = 0;
    for (auto& block : function.blocks) {
        for (ValueID id : block.values) {
            if (next == valueCount || id.index() >= valueCount)
                return false;
            ids[next] = id;
            opcodes[next++] = valueOpcodes[id.index()];
        }
        contents.terminatorKinds |= 1u << static_cast<unsigned>(block.terminator.kind);
    }
    indexedValueCount = next;
    return true;
}

void Functions::indexValues(size_t index)
{
    uint32_t first = m_firstIndexedValue[index];
    uint32_t valueCount = m_valueCount[index];
    Contents contents;
    if (!fillValueIndexRows(m_functions[index], m_indexedValues.items<&IndexedValue::id>().subspan(first, valueCount), m_indexedValues.items<&IndexedValue::opcode>().subspan(first, valueCount), m_valueOpcodes.mutableSpan().subspan(first, valueCount), contents, m_indexedValueCount[index]))
        return;
    m_contents[index] = contents;
    m_isIndexed[index] = 1;
}

void Functions::ValueIndexRows::append(size_t index, const Function& function)
{
    size_t first = m_valueOpcodes.size();
    size_t valueCount = function.values.size();
    m_ids.grow(first + valueCount);
    m_opcodes.grow(first + valueCount);
    m_valueOpcodes.grow(first + valueCount);
    Entry entry { static_cast<uint32_t>(index), static_cast<uint32_t>(first), static_cast<uint32_t>(valueCount), 0, { } };
    if (fillValueIndexRows(function, m_ids.mutableSpan().subspan(first), m_opcodes.mutableSpan().subspan(first), m_valueOpcodes.mutableSpan().subspan(first), entry.contents, entry.indexedValueCount))
        m_entries.append(entry);
}

void Functions::adoptValueIndexRows(std::span<const ValueIndexRows> allRows)
{
    size_t count = m_functions.size();
    m_isIndexed.fill(0, count);
    m_valueCount.fill(0, count);
    m_firstIndexedValue.fill(0, count);
    m_indexedValueCount.grow(count);
    m_contents.grow(count);
    m_indexedValues.clear();
    m_valueOpcodes.shrink(0);
    m_hasInvalidatedFunctions = 1;
    struct Source {
        const ValueIndexRows* rows { nullptr };
        const ValueIndexRows::Entry* entry { nullptr };
    };
    Vector<Source> sources(count);
    for (auto& rows : allRows) {
        for (auto& entry : rows.m_entries)
            sources[entry.function] = { &rows, &entry };
    }
    uint32_t end = 0;
    for (size_t i = 0; i < count; ++i) {
        if (!sources[i].entry)
            continue;
        m_firstIndexedValue[i] = end;
        m_valueCount[i] = sources[i].entry->valueCount;
        end += sources[i].entry->valueCount;
    }
    m_indexedValues.reserveCapacity(end + end / 4);
    m_valueOpcodes.reserveCapacity(end + end / 4);
    m_indexedValues.growWithoutInitializing(end);
    m_valueOpcodes.grow(end);
    forEachIndexInParallel(count, [&](unsigned i) {
        auto [rows, entry] = sources[i];
        if (!entry)
            return;
        uint32_t first = m_firstIndexedValue[i];
        memcpySpan(m_indexedValues.items<&IndexedValue::id>().subspan(first), rows->m_ids.span().subspan(entry->first, entry->indexedValueCount));
        memcpySpan(m_indexedValues.items<&IndexedValue::opcode>().subspan(first), rows->m_opcodes.span().subspan(entry->first, entry->indexedValueCount));
        memcpySpan(m_valueOpcodes.mutableSpan().subspan(first), rows->m_valueOpcodes.span().subspan(entry->first, entry->valueCount));
        m_indexedValueCount[i] = entry->indexedValueCount;
        m_contents[i] = entry->contents;
        m_isIndexed[i] = 1;
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
