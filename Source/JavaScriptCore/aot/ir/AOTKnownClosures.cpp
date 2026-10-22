/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTKnownClosures.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCFG.h"
#include "AOTSchemaResultRules.h"
#include "AOTThreads.h"
#include <algorithm>
#include <functional>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct SameValueRules {
    using Result = ValueID;

    ValueID sameAs(const Instruction& instruction, uint32_t operandIndex) { return instruction.operand(operandIndex); }
    ValueID cast(const Value& value, uint32_t operandIndex, TypeID = { }) { return value.operand(operandIndex); }

    ValueID none(const Instruction&) { return { }; }
    ValueID constant(const Value&) { return { }; }
    ValueID unknown(const Instruction&) { return { }; }
    ValueID declaredParameter(const Value&) { return { }; }
    ValueID natural(const Instruction&, const auto&) { return { }; }
    ValueID allocation(const Value&, const auto&...) { return { }; }
    ValueID declared(const Instruction&, const auto&...) { return { }; }
    ValueID declaredContents(const Value&, const auto&...) { return { }; }
    ValueID declaredResult(const Value&, const auto&...) { return { }; }
    ValueID resultOperand(const Value&, uint32_t) { return { }; }
};

struct FilledHolder {
    ClosureHolder holder;
    FunctionID function;

    friend bool operator==(const FilledHolder&, const FilledHolder&) = default;
    friend auto operator<=>(const FilledHolder&, const FilledHolder&) = default;
};

ClosureHolder::Kind holderKindOf(MethodKind kind)
{
    switch (kind) {
    case MethodKind::Method:
        return ClosureHolder::Kind::Member;
    case MethodKind::Getter:
        return ClosureHolder::Kind::Getter;
    case MethodKind::Setter:
        return ClosureHolder::Kind::Setter;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

class ClosureCollector {
public:
    ClosureCollector(const Program& program, const Function& function, Vector<FilledHolder>& found)
        : m_program(program)
        , m_function(function)
        , m_found(found)
    {
    }

    void run()
    {
        for (const Block& block : m_function.blocks) {
            for (ValueID id : block.values)
                visit(m_function.at(id));
        }
    }

private:
    FunctionID closureOf(ValueID id) const
    {
        if (!id)
            return { };
        const Value& definition = m_function.at(definitionThroughSameValues(m_function, id));
        return definition.opcode == Opcode::NewClosure ? definition.function() : FunctionID();
    }

    void add(ClosureHolder holder, ValueID stored)
    {
        if (FunctionID function = closureOf(stored))
            m_found.append({ holder, function });
    }

    void addMember(ClosureHolder::Kind kind, LayoutID layout, PropertyKeyID key, ValueID stored)
    {
        add(ClosureHolder::member(kind, layout, ClassSide::Prototype, key), stored);
    }

    void addField(LayoutID id, uint32_t field, ValueID stored)
    {
        const Layout& layout = m_program.at(id);
        if (field >= layout.fields.size())
            return;
        if (layout.isEnvironment())
            add(ClosureHolder::environmentField(id, field), stored);
        else if (PropertyKeyID key = layout.fields[field].key)
            addMember(ClosureHolder::Kind::Member, id, key, stored);
    }

    bool isThis(ValueID id) const
    {
        std::optional<unsigned> index = m_function.thisParameterIndex();
        const CompactList<ValueID>& parameters = m_function.at(entryBlock).parameters;
        return index && *index < parameters.size() && parameters[*index] == id;
    }

    void addMemberOfReceiver(ClosureHolder::Kind kind, ValueID receiver, PropertyKeyID key, ValueID stored)
    {
        if (!key || !closureOf(stored))
            return;
        ValueID definition = definitionThroughSameValues(m_function, receiver);
        if (m_function.at(definition).opcode == Opcode::NewObject) {
            addMember(kind, m_function.at(definition).layout(), key, stored);
            return;
        }
        if (!isThis(definition))
            return;
        for (LayoutID layout : membersOf(m_program, m_program.at(m_function.thisType.type)))
            addMember(kind, layout, key, stored);
    }

    void addInitialFields(const Value& allocation)
    {
        std::span<const uint32_t> fields = allocation.fields();
        std::span<const ValueID> values = allocation.variadicOperands();
        for (size_t index = 0; index < std::min(fields.size(), values.size()); ++index)
            addField(allocation.layout(), fields[index], values[index]);
    }

    void addCaptures(const Value& closure)
    {
        std::span<const ValueID> captures = closure.variadicOperands();
        for (uint32_t index = 0; index < captures.size(); ++index)
            add(ClosureHolder::capture(closure.function(), index), captures[index]);
    }

    void visit(const Value& value)
    {
        switch (value.opcode) {
        case Opcode::PutModuleVariable:
            add(ClosureHolder::variable(value.variable()), value.valueOperand());
            return;
        case Opcode::PutEnvironmentField:
            addField(value.layout(), value.field(), value.valueOperand());
            return;
        case Opcode::NewClosure:
            addCaptures(value);
            return;
        case Opcode::DefineMethod:
            if (value.key())
                add(ClosureHolder::member(holderKindOf(value.methodKind()), value.layout(), value.side(), value.key()), value.functionOperand());
            return;
        case Opcode::DefineGetter:
            addMemberOfReceiver(ClosureHolder::Kind::Getter, value.objectOperand(), value.key(), value.functionOperand());
            return;
        case Opcode::DefineSetter:
            addMemberOfReceiver(ClosureHolder::Kind::Setter, value.objectOperand(), value.key(), value.functionOperand());
            return;
        case Opcode::NewObject:
            addInitialFields(value);
            return;
        case Opcode::PropSet:
        case Opcode::PropDefine:
            addMemberOfReceiver(ClosureHolder::Kind::Member, value.objectOperand(), value.key(), value.valueOperand());
            return;
        default:
            return;
        }
    }

    const Program& m_program;
    const Function& m_function;
    Vector<FilledHolder>& m_found;
};

} // anonymous namespace

ValueID sameValueOperandOf(const Value& value)
{
    SameValueRules rules;
    return applyResultRule(rules, value, 0, ResultSide::Unproven);
}

ValueID definitionThroughSameValues(const Function& function, ValueID id)
{
    while (ValueID operand = sameValueOperandOf(function.at(id)))
        id = operand;
    return id;
}

KnownClosures::KnownClosures(const Program& program, BuildContext& build)
{
    const ParallelLoops& loops = build.parallelLoops();
    Vector<FilledHolder> filled = collectInParallel<FilledHolder>(loops, program.functions.size(), [&](unsigned index, Vector<FilledHolder>& found) {
        if (program.functions[index].hasBody())
            ClosureCollector(program, program.functions[index], found).run();
    });
    for (size_t index = 0; index < program.variables.size(); ++index) {
        const Variable& variable = program.variables[index];
        if (variable.initialization == VariableInitialization::Function && variable.function)
            filled.append({ ClosureHolder::variable(VariableID(index)), variable.function });
    }
    parallelSort(loops, filled.mutableSpan(), std::less<FilledHolder> { });
    filled.shrink(std::ranges::unique(filled).begin() - filled.begin());

    for (const FilledHolder& entry : filled) {
        m_holders.append(entry.holder);
        m_closures.append(entry.function);
    }
    parallelSort(loops, filled.mutableSpan(), [](const FilledHolder& a, const FilledHolder& b) {
        return std::pair(a.function, a.holder) < std::pair(b.function, b.holder);
    });
    for (const FilledHolder& entry : filled) {
        m_functions.append(entry.function);
        m_holdersByFunction.append(entry.holder);
    }

    for (size_t index = 0; index < program.layouts.size(); ++index) {
        const Layout& layout = program.layouts[index];
        if (layout.flags.contains(LayoutFlags::HasHeritage) && !layout.base)
            m_classesWithUnknownBase.append(LayoutID(index));
    }
}

std::span<const FunctionID> KnownClosures::closuresIn(ClosureHolder holder) const
{
    auto range = std::ranges::equal_range(m_holders, holder);
    return m_closures.span().subspan(range.begin() - m_holders.begin(), range.size());
}

std::span<const ClosureHolder> KnownClosures::holdersOf(FunctionID function) const
{
    auto range = std::ranges::equal_range(m_functions, function);
    return m_holdersByFunction.span().subspan(range.begin() - m_functions.begin(), range.size());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
