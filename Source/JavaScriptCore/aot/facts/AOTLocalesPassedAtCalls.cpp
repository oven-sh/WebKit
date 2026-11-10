/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLocalesPassedAtCalls.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTOpcodeSets.h"
#include "AOTSort.h"
#include "IntlObject.h"
#include <algorithm>
#include <tuple>
#include <wtf/HashMap.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet callsInLocalesPassed { Opcode::Call, Opcode::CallList, Opcode::Construct, Opcode::ConstructList };
static constexpr OpcodeSet constructionsInLocalesPassed { Opcode::Construct, Opcode::ConstructList };
static constexpr OpcodeSet callsThatSpellOutArgumentsInLocalesPassed { Opcode::Call, Opcode::Construct };

RowsThatTakeLocales::RowsThatTakeLocales(const BuiltinTable& builtins)
    : rows(builtins.builtinCount())
{
    for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
        BuiltinID row = builtins.canonicalID(BuiltinID(index));
        for (const BuiltinOverload& overload : builtins.overloadsOf(BuiltinID(index))) {
            auto look = [&](const BuiltinParameter& parameter) {
                if (!parameter.roles.containsAny({ BuiltinOperandRole::Locale, BuiltinOperandRole::MayReplaceLocale }))
                    return;
                rows.quickSet(row.index());
                operands.append(LocaleOperandOfRow { row, overload.mode, parameter.operand, !parameter.roles.contains(BuiltinOperandRole::Locale) });
            };
            if (const BuiltinParameter* receiver = overload.receiver.get())
                look(*receiver);
            std::ranges::for_each(overload.parameters, look);
        }
    }
    AOT::sort(operands, { }, [](const LocaleOperandOfRow& taken) {
        return std::tuple(taken.row, taken.mode, taken.operand.kind(), taken.operand.isReceiver() ? 0u : taken.operand.argumentIndex());
    });
    removeRepeatedElements(operands);
}

static ValueID valueBehindWhatHandsOnInLocalesPassed(const BodyIndex& index, ValueID id)
{
    while (id && !index.isParameter(id)) {
        BodyValue definition = index.value(id);
        if (!inputPassThroughOpcodes.contains(definition.opcode()) || !definition.operand(0))
            break;
        id = definition.operand(0);
    }
    return id;
}

static StringID textWrittenAtInLocalesPassed(const BodyIndex& index, ValueID value)
{
    ValueID written = value ? valueBehindWhatHandsOnInLocalesPassed(index, value) : ValueID();
    bool isText = written && !index.isParameter(written) && index.opcodeOf(written) == Opcode::String;
    return isText ? index.value(written).as<Opcode::String>().string() : StringID();
}

namespace {

class TextsOfImmutableVariables {
    WTF_MAKE_NONCOPYABLE(TextsOfImmutableVariables);
public:
    TextsOfImmutableVariables(const Program& program, const BodyIndexTable& bodies)
        : m_program(program)
        , m_bodies(bodies)
    {
    }

    StringID textIn(VariableID id)
    {
        return m_known.ensure(id.index(), [&] {
            return find(id);
        }).iterator->value;
    }

private:
    StringID find(VariableID) const;

    const Program& m_program;
    const BodyIndexTable& m_bodies;
    HashMap<uint32_t, StringID, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_known;
};

StringID TextsOfImmutableVariables::find(VariableID id) const
{
    const Variable& variable = m_program.at(id);
    if (!variable.module || variable.initialization != VariableInitialization::Uninitialized || !variable.flags.contains(VariableFlags::IsImmutable) || variable.flags.containsAny({ VariableFlags::IsReplaceable, VariableFlags::HoldsMock, VariableFlags::OriginalMayRun }))
        return { };
    FunctionID body = m_program.at(variable.module).body;
    const BodyIndex* index = body ? m_bodies.find(body) : nullptr;
    if (!index || !index->holdsAnyOf(OpcodeSet { Opcode::PutModuleVariable }))
        return { };

    ValueID stored;
    unsigned numberOfStores = 0;
    for (uint32_t number = 0; number < index->numberOfValues(); ++number) {
        if (index->isParameter(ValueID(number)) || index->opcodeOf(ValueID(number)) != Opcode::PutModuleVariable)
            continue;
        auto store = index->value(ValueID(number)).as<Opcode::PutModuleVariable>();
        if (store.variable() != id)
            continue;
        ++numberOfStores;
        stored = store.valueOperand();
    }
    return numberOfStores == 1 ? textWrittenAtInLocalesPassed(*index, stored) : StringID();
}

class LocalesPassedAtCall {
    WTF_MAKE_NONCOPYABLE(LocalesPassedAtCall);
public:
    LocalesPassedAtCall(const Program& program, const BodyIndex& index, const LinkedCallOfBuiltin& listed, TextsOfImmutableVariables& variables, Vector<LocaleUse>& uses)
        : m_program(program)
        , m_function(program.at(listed.caller))
        , m_index(index)
        , m_listed(listed)
        , m_call(index.value(listed.site))
        , m_variables(variables)
        , m_uses(uses)
    {
    }

    void look(std::span<const LocaleOperandOfRow>);

private:
    Witness witnessAt(const BodyValue& value) const { return { m_listed.caller.index(), value.id().index(), m_function.locationOf(value) }; }
    bool isHandedToCallAlone(ValueID list) const;
    void note(LocaleUseKind kind, StringID tag, const Witness& witness) { m_uses.append(LocaleUse { kind, tag, m_listed.row, witness }); }
    void noteUnknown(const Witness& witness) { note(LocaleUseKind::Unknown, { }, witness); }
    void noteTag(StringID, const Witness&);
    void noteList(const BodyValue& list, const Witness&);
    void noteArms(ValueID parameter);
    void noteLocale(ValueID operand);

    const Program& m_program;
    const Function& m_function;
    const BodyIndex& m_index;
    const LinkedCallOfBuiltin& m_listed;
    const BodyValue m_call;
    TextsOfImmutableVariables& m_variables;
    Vector<LocaleUse>& m_uses;
};

bool LocalesPassedAtCall::isHandedToCallAlone(ValueID list) const
{
    for (ValueID value = list;;) {
        ValueID user = m_index.onlyUsingValue(value);
        if (!user)
            return false;
        if (user == m_call.id())
            return true;
        BodyValue operation = m_index.value(user);
        if (!inputPassThroughOpcodes.contains(operation.opcode()) || operation.operand(0) != value)
            return false;
        value = user;
    }
}

void LocalesPassedAtCall::noteTag(StringID text, const Witness& witness)
{
    Vector<char16_t> buffer;
    if (text && isStructurallyValidLanguageTag(m_program.strings.viewOf(text, buffer)))
        note(LocaleUseKind::Constant, text, witness);
    else
        noteUnknown(witness);
}

void LocalesPassedAtCall::noteList(const BodyValue& list, const Witness& witness)
{
    if (!isHandedToCallAlone(list.id()))
        return noteUnknown(witness);

    if (list.opcode() == Opcode::Data) {
        DataID data = list.as<Opcode::Data>().data();
        const auto* array = data ? m_program.at(data).root.tryAs<DataNode::Array>() : nullptr;
        if (!array)
            return noteUnknown(witness);
        if (array->elements.isEmpty())
            return note(LocaleUseKind::HostDefault, { }, witness);
        for (const DataNode& element : array->elements) {
            const auto* text = element.tryAs<DataNode::String>();
            noteTag(text ? text->value : StringID(), witness);
        }
        return;
    }

    auto made = list.as<Opcode::NewArray>();
    if (!made.holes().empty())
        return noteUnknown(witness);
    if (made.elementsOperands().empty())
        return note(LocaleUseKind::HostDefault, { }, witness);
    for (ValueID element : made.elementsOperands())
        noteTag(textWrittenAtInLocalesPassed(m_index, element), witness);
}

void LocalesPassedAtCall::noteArms(ValueID parameter)
{
    Witness witness = witnessAt(m_call);
    BlockID block = m_index.blockOf(parameter);
    std::optional<uint32_t> position = m_index.block(block).indexOfParameter(parameter);
    uint32_t numberOfEdges = m_index.numberOfIncomingEdges(block);
    if (!position || !numberOfEdges || numberOfEdges == BodyIndex::maxCountedIncomingEdges || m_index.isLoopHead(block) || m_index.isEnteredByHandlerEdge(block))
        return noteUnknown(witness);

    uint32_t numberOfEdgesFound = 0;
    for (BlockID from : m_index.blocksInDominatorOrder()) {
        for (BodyTarget edge : m_index.terminator(from).targets()) {
            if (edge.block() != block)
                continue;
            ++numberOfEdgesFound;
            noteTag(textWrittenAtInLocalesPassed(m_index, *position < edge.arguments().size() ? edge.arguments()[*position] : ValueID()), witness);
        }
    }
    if (numberOfEdgesFound != numberOfEdges)
        noteUnknown(witness);
}

void LocalesPassedAtCall::noteLocale(ValueID operand)
{
    ValueID behind = valueBehindWhatHandsOnInLocalesPassed(m_index, operand);
    if (m_index.isParameter(behind))
        return noteArms(behind);

    BodyValue passed = m_index.value(behind);
    Witness witness = witnessAt(passed.hasPosition() ? passed : m_call);
    switch (passed.opcode()) {
    case Opcode::Undefined:
        return note(LocaleUseKind::HostDefault, { }, witness);
    case Opcode::String:
        return noteTag(passed.as<Opcode::String>().string(), witness);
    case Opcode::GetModuleVariable:
        return noteTag(m_variables.textIn(passed.as<Opcode::GetModuleVariable>().variable()), witness);
    case Opcode::NewArray:
    case Opcode::Data:
        return noteList(passed, witness);
    default:
        return noteUnknown(witness);
    }
}

void LocalesPassedAtCall::look(std::span<const LocaleOperandOfRow> operands)
{
    bool spellsOutArguments = callsThatSpellOutArgumentsInLocalesPassed.contains(m_call.opcode());
    std::span<const ValueID> arguments = m_call.variadicOperands();
    for (const LocaleOperandOfRow& taken : operands) {
        bool isPassed = spellsOutArguments && taken.operand.kind() == BuiltinOperand::Kind::Argument && taken.operand.argumentIndex() < arguments.size();
        ValueID passed = isPassed ? valueBehindWhatHandsOnInLocalesPassed(m_index, arguments[taken.operand.argumentIndex()]) : ValueID();
        bool isUndefined = passed && !m_index.isParameter(passed) && m_index.opcodeOf(passed) == Opcode::Undefined;
        if (taken.mayReplaceLocale && spellsOutArguments && (!isPassed || isUndefined))
            continue;
        if (taken.mayReplaceLocale || !spellsOutArguments || taken.operand.kind() != BuiltinOperand::Kind::Argument)
            noteUnknown(witnessAt(m_call));
        else if (taken.operand.argumentIndex() >= arguments.size())
            note(LocaleUseKind::HostDefault, { }, witnessAt(m_call));
        else
            noteLocale(arguments[taken.operand.argumentIndex()]);
    }
}

} // anonymous namespace

LocalesPassedAtCalls localesPassedAtCalls(const Program& program, const BodyIndexTable& bodies, const RowsThatTakeLocales& rows, std::span<const LinkedCallOfBuiltin> calls)
{
    LocalesPassedAtCalls passed;
    TextsOfImmutableVariables variables(program, bodies);
    for (const LinkedCallOfBuiltin& listed : calls) {
        if (!rows.rows.get(listed.row.index()))
            continue;
        const BodyIndex* index = listed.caller ? bodies.find(listed.caller) : nullptr;
        bool isOperation = index && listed.site && listed.site.index() < index->numberOfValues() && !index->isParameter(listed.site);
        Opcode opcode = isOperation ? index->opcodeOf(listed.site) : Opcode::Undefined;
        if (!callsInLocalesPassed.contains(opcode) || constructionsInLocalesPassed.contains(opcode) != listed.isConstruction) {
            passed.rowsCalledWhereNobodySeesOperands.set(listed.row.index());
            continue;
        }
        passed.rowsWithCallsLookedAt.set(listed.row.index());
        BuiltinCallMode mode = listed.isConstruction ? BuiltinCallMode::Construct : BuiltinCallMode::Call;
        auto ofRow = std::ranges::equal_range(rows.operands, std::tuple(listed.row, mode), { }, [](const LocaleOperandOfRow& taken) {
            return std::tuple(taken.row, taken.mode);
        });
        LocalesPassedAtCall(program, *index, listed, variables, passed.uses).look(std::span<const LocaleOperandOfRow>(ofRow.begin(), ofRow.end()));
    }
    return passed;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
