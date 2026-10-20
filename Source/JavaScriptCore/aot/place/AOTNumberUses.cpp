/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSlotPlanSteps.h"

#if ENABLE(AOT)

#include "AOTFunctionAnalyses.h"
#include "AOTSortedSet.h"
#include "AOTUseLists.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet arithmeticOpcodes { Opcode::Add, Opcode::Sub, Opcode::Mul };
static constexpr OpcodeSet divisionOpcodes { Opcode::Div, Opcode::Pow };
static constexpr OpcodeSet comparisonOpcodes { Opcode::Less, Opcode::LessEq, Opcode::Greater, Opcode::GreaterEq, Opcode::StrictEq };
static constexpr OpcodeSet conversionOpcodes { Opcode::Negate, Opcode::ToNumber, Opcode::ToNumeric, Opcode::Upcast, Opcode::Claim, Opcode::Predict, Opcode::AssumeType };

static bool isNonIntegerNumber(const Type& type)
{
    return type.isNumber() && !type.isInteger();
}

static bool isStoredOperand(const Value& store, uint32_t operandIndex)
{
    return operandInfoOf(store.opcode, operandIndex).retainedIn == RetainedIn::Operand;
}

static const Field& fieldDeclarationAt(const Program& program, LayoutID layout, FieldIndex index)
{
    return fieldAt(program, declarationOf(program, { layout, index }));
}

bool isPredictedInt32(const Program& program, const Field& field)
{
    if (!field.declared.predicted || program.at(field.declared.type).isInteger())
        return false;
    return program.at(field.declared.predicted).withoutNullish().isInt32();
}

bool isPredictedInt32(const Program& program, PropertyKeyID key, LayoutSetID receivers)
{
    if (!receivers)
        return false;
    return std::ranges::any_of(program.at(receivers).members, [&](LayoutID member) {
        auto found = program.at(member).findField(key);
        return found && isPredictedInt32(program, fieldDeclarationAt(program, member, *found));
    });
}

namespace {

class NumberUseCounter {
public:
    NumberUseCounter(const Program&, FunctionAnalyses&, const NumberSources&);

    void propagateSources();
    void countUses(Vector<SlotAccessUse>&) const;

private:
    using SourceSet = SortedSet<uint32_t, 1>;

    const Type& typeOf(ValueID id) const { return m_program.typeOf(m_function, id); }
    bool isTracked(ValueID id) const { return !m_sources[id.index()].isEmpty(); }
    Vector<ValueID> trackedValues() const;
    ValueID targetParameterOf(const Use&) const;

    NumberUse numberUseWithOther(ValueID other, NumberUse ifInteger) const;
    NumberUse numberUseByField(LayoutID, FieldIndex) const;
    NumberUse numberUseByAllocation(const Value& allocation, uint32_t operandIndex) const;
    NumberUse numberUseByStore(const Value& store, uint32_t operandIndex) const;
    NumberUse numberUseByCall(const Value& call, ValueID used) const;
    NumberUse numberUseBy(const Value& user, ValueID used, uint32_t operandIndex) const;
    NumberUse numberUseBy(ValueID used, const Use&) const;

    bool inheritSources(ValueID to, ValueID from);
    bool inheritSourcesOfOperands(ValueID user);

    const Program& m_program;
    const Function& m_function;
    const UseLists& m_useLists;
    Vector<SourceSet> m_sources;
};

NumberUseCounter::NumberUseCounter(const Program& program, FunctionAnalyses& analyses, const NumberSources& sources)
    : m_program(program)
    , m_function(analyses.function())
    , m_useLists(analyses.useLists())
    , m_sources(m_function.values.size())
{
    for (auto& entry : sources)
        m_sources[entry.key.index()] = SourceSet(entry.value.span());
}

Vector<ValueID> NumberUseCounter::trackedValues() const
{
    Vector<ValueID> result;
    for (uint32_t index = 0; index < m_sources.size(); ++index) {
        if (!m_sources[index].isEmpty())
            result.append(ValueID(index));
    }
    return result;
}

ValueID NumberUseCounter::targetParameterOf(const Use& use) const
{
    auto& target = m_function.at(use.block).terminator.targets[use.targetIndex];
    return m_function.at(target.block).parameters[use.operandIndex];
}

NumberUse NumberUseCounter::numberUseWithOther(ValueID other, NumberUse ifInteger) const
{
    if (isTracked(other) || typeOf(other).isInteger())
        return ifInteger;
    return isNonIntegerNumber(typeOf(other)) ? NumberUse::AsDouble : NumberUse::AsInteger;
}

NumberUse NumberUseCounter::numberUseByField(LayoutID id, FieldIndex index) const
{
    const Field& field = fieldDeclarationAt(m_program, id, index);
    if (!m_program.at(id).isEnvironment() && isPredictedInt32(m_program, field))
        return NumberUse::AsInteger;
    return isNonIntegerNumber(m_program.at(field.declared.type)) ? NumberUse::AsDouble : NumberUse::AsInteger;
}

NumberUse NumberUseCounter::numberUseByAllocation(const Value& allocation, uint32_t operandIndex) const
{
    auto initialValueIndex = allocation.variadicOperandIndex(operandIndex);
    if (!initialValueIndex)
        return NumberUse::AsInteger;
    return numberUseByField(allocation.layout(), allocation.fields()[*initialValueIndex]);
}

NumberUse NumberUseCounter::numberUseByStore(const Value& store, uint32_t operandIndex) const
{
    if (storedOf(store.opcode) != operandIndex)
        return NumberUse::AsInteger;
    PropertyKeyID key = store.key();
    LayoutSetID receivers = store.receivers();
    if (!receivers || isPredictedInt32(m_program, key, receivers))
        return NumberUse::AsInteger;
    bool isInSomeMember = false;
    for (LayoutID member : m_program.at(receivers).members) {
        auto found = m_program.at(member).findField(key);
        if (!found)
            continue;
        if (!isNonIntegerNumber(m_program.at(fieldDeclarationAt(m_program, member, *found).declared.type)))
            return NumberUse::AsInteger;
        isInSomeMember = true;
    }
    return isInSomeMember ? NumberUse::AsDouble : NumberUse::AsInteger;
}

NumberUse NumberUseCounter::numberUseByCall(const Value& call, ValueID used) const
{
    auto& callee = m_program.at(call.function());
    if (callee.numberOfEntryParameters() != call.numberOfOperands())
        return NumberUse::AsInteger;
    for (uint32_t i = 0; i < call.numberOfOperands(); ++i) {
        if (call.operand(i) != used)
            continue;
        const DeclaredType* declared = callee.declaredEntryParameter(i);
        if (!declared || !isNonIntegerNumber(m_program.at(declared->type)))
            return NumberUse::AsInteger;
    }
    return NumberUse::AsDouble;
}

NumberUse NumberUseCounter::numberUseBy(const Value& user, ValueID used, uint32_t operandIndex) const
{
    Opcode opcode = user.opcode;
    if (arithmeticOpcodes.contains(opcode))
        return isNonIntegerNumber(m_program.at(user.type)) ? numberUseWithOther(user.operand(1 - operandIndex), NumberUse::PassedOn) : NumberUse::AsInteger;
    if (divisionOpcodes.contains(opcode))
        return typeOf(user.operand(0)).isNumber() && typeOf(user.operand(1)).isNumber() ? NumberUse::AsDouble : NumberUse::AsInteger;
    if (comparisonOpcodes.contains(opcode))
        return numberUseWithOther(user.operand(1 - operandIndex), NumberUse::AsInteger);
    if (conversionOpcodes.contains(opcode))
        return isNonIntegerNumber(m_program.at(user.type)) ? NumberUse::PassedOn : NumberUse::AsInteger;
    if (opcode == Opcode::PutModuleVariable)
        return isNonIntegerNumber(m_program.at(m_program.at(user.variable()).declared.type)) ? NumberUse::AsDouble : NumberUse::AsInteger;
    if (opcode == Opcode::PutEnvironmentField)
        return isStoredOperand(user, operandIndex) ? numberUseByField(user.layout(), user.field()) : NumberUse::AsInteger;
    if (opcode == Opcode::NewObject)
        return numberUseByAllocation(user, operandIndex);
    if (familyOf(opcode) == OperationFamily::SlotAccess)
        return numberUseByStore(user, operandIndex);
    if (opcode == Opcode::CallDirect)
        return numberUseByCall(user, used);
    return NumberUse::Unknown;
}

NumberUse NumberUseCounter::numberUseBy(ValueID used, const Use& use) const
{
    switch (use.kind) {
    case Use::Kind::Operand:
        return numberUseBy(m_function.at(use.user), used, use.operandIndex);
    case Use::Kind::TerminatorOperand:
        if (m_function.at(use.block).terminator.kind != TerminatorKind::Return)
            return NumberUse::Unknown;
        return m_function.results.size() == 1 && isNonIntegerNumber(m_program.at(m_function.result().type)) ? NumberUse::AsDouble : NumberUse::AsInteger;
    case Use::Kind::EdgeArgument:
        return isNonIntegerNumber(typeOf(targetParameterOf(use))) ? NumberUse::PassedOn : NumberUse::AsInteger;
    case Use::Kind::HandlerArgument:
    case Use::Kind::BlockSubject:
        return NumberUse::Unknown;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool NumberUseCounter::inheritSources(ValueID to, ValueID from)
{
    return m_sources[to.index()].unite(m_sources[from.index()]);
}

bool NumberUseCounter::inheritSourcesOfOperands(ValueID user)
{
    bool changed = false;
    m_function.at(user).forEachOperand([&](ValueID operand, uint32_t) {
        if (isTracked(operand))
            changed |= inheritSources(user, operand);
    });
    return changed;
}

void NumberUseCounter::propagateSources()
{
    Vector<ValueID> frontier = trackedValues();
    while (!frontier.isEmpty()) {
        Vector<ValueID> next;
        for (ValueID used : frontier) {
            for (const Use& use : m_useLists.usesOf(used)) {
                if (numberUseBy(used, use) != NumberUse::PassedOn)
                    continue;
                if (use.kind == Use::Kind::EdgeArgument) {
                    ValueID parameter = targetParameterOf(use);
                    if (inheritSources(parameter, used))
                        next.append(parameter);
                } else if (inheritSourcesOfOperands(use.user))
                    next.append(use.user);
            }
        }
        std::ranges::sort(next);
        removeRepeatedElements(next);
        frontier = WTF::move(next);
    }
}

void NumberUseCounter::countUses(Vector<SlotAccessUse>& accesses) const
{
    for (ValueID used : trackedValues()) {
        uint32_t integerUses = 0;
        uint32_t doubleUses = 0;
        for (const Use& use : m_useLists.usesOf(used)) {
            switch (numberUseBy(used, use)) {
            case NumberUse::PassedOn:
                break;
            case NumberUse::AsDouble:
                ++doubleUses;
                break;
            case NumberUse::AsInteger:
            case NumberUse::Unknown:
                ++integerUses;
                break;
            }
        }
        for (uint32_t source : m_sources[used.index()]) {
            accesses[source].integerUses += integerUses;
            accesses[source].doubleUses += doubleUses;
        }
    }
}

} // anonymous namespace

void countNumberUses(const SlotPlanContext& context, FunctionAnalyses& analyses, NumberSources&& sources, Vector<SlotAccessUse>& accessesOfFunction)
{
    NumberUseCounter counter(context.program, analyses, sources);
    counter.propagateSources();
    counter.countUses(accessesOfFunction);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
