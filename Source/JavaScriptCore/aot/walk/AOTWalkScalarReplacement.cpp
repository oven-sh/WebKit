/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkScalarReplacement.h"

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include "AOTInliningRules.h"
#include "AOTLinkTables.h"
#include "AOTOpcodeSets.h"
#include "AOTPlan.h"
#include "AOTSlotPlan.h"
#include "AOTThinSummary.h"
#include "AOTTypeTable.h"
#include <optional>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t mostOperationsTakenApart = 40;
static constexpr uint32_t mostCallsFollowed = 3;

namespace {

class FatesOfValuesInBody {
public:
    FatesOfValuesInBody(const Walker& walker, FunctionID function, const BodyIndex& index, uint32_t callsFollowed)
        : m_walker(walker)
        , m_function(function)
        , m_index(index)
        , m_callsFollowed(callsFollowed)
    {
        m_rootsOfValues.fill(UINT32_MAX, index.numberOfValues());
    }

    uint32_t addRoot(ValueID value)
    {
        uint32_t root = static_cast<uint32_t>(m_fates.size());
        m_rootsOfValues[value.index()] = root;
        m_fates.append(FateOfValue::OnlyTakenApart);
        return root;
    }
    FateOfValue fateOf(uint32_t root) const { return m_fates[root]; }

    template<typename Functor>
    void follow(const Functor& noteOperation)
    {
        for (uint32_t number = 0; number < m_index.numberOfBlocks(); ++number) {
            BodyBlock block = m_index.block(BlockID(number));
            letEscape(block.subject());
            for (BodyValue user : block.values()) {
                user.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                    if (uint32_t root = rootOf(operand); root != UINT32_MAX && m_fates[root] != FateOfValue::Escapes && !onlyTakesApart(user, operandIndex))
                        m_fates[root] = FateOfValue::Escapes;
                });
                noteOperation(user);
            }
            BodyTerminator end = block.terminator();
            end.forEachOperand([&](ValueID operand, uint32_t) {
                uint32_t root = rootOf(operand);
                if (root != UINT32_MAX && m_fates[root] != FateOfValue::Escapes)
                    m_fates[root] = end.kind() == TerminatorKind::Return || end.kind() == TerminatorKind::ReturnCall ? FateOfValue::Returned : FateOfValue::Escapes;
            });
            block.forEachOutgoingEdge([&](BodyTarget target, bool) {
                for (ValueID argument : target.arguments())
                    letEscape(argument);
            });
            m_index.forEachEdgeResult(block.id(), [&](const BodyEdgeResult& result) {
                if (uint32_t root = rootOf(result.subject); root != UINT32_MAX)
                    m_rootsOfValues[result.parameter.index()] = root;
            });
        }
    }

private:
    uint32_t rootOf(ValueID value) const { return value ? m_rootsOfValues[value.index()] : UINT32_MAX; }

    void letEscape(ValueID value)
    {
        if (uint32_t root = rootOf(value); root != UINT32_MAX)
            m_fates[root] = FateOfValue::Escapes;
    }

    bool isWrittenNullish(ValueID value) const
    {
        ValueID written = m_index.stripCasts(value);
        return !m_index.isParameter(written) && (m_index.opcodeOf(written) == Opcode::Null || m_index.opcodeOf(written) == Opcode::Undefined);
    }

    bool onlyTakesApart(const BodyValue& user, uint32_t operandIndex)
    {
        Opcode opcode = user.opcode();
        if (inputPassThroughOpcodes.contains(opcode)) {
            m_rootsOfValues[user.id().index()] = rootOf(user.operand(operandIndex));
            return !operandIndex;
        }
        if (opcode == Opcode::ToBoolean)
            return true;
        if (opcode == Opcode::TypeTest)
            return m_walker.types().isSubset(user.as<Opcode::TypeTest>().testedType(), TypeTable::nullish);
        if (allEqualities.contains(opcode))
            return isWrittenNullish(user.operand(!operandIndex));
        if (opcode == Opcode::PropGet)
            return operandIndex == OperandIndex::PropGet::object && !user.isFreeName();
        if (opcode == Opcode::PropSet)
            return operandIndex == OperandIndex::PropSet::object && !user.isFreeName();
        if (opcode == Opcode::ElemGet) {
            ValueID key = m_index.stripCasts(user.propertyOperand());
            return operandIndex == OperandIndex::ElemGet::object && !m_index.isParameter(key) && m_index.opcodeOf(key) == Opcode::Integer;
        }
        if (opcode == Opcode::IteratorOpen) {
            auto open = user.as<Opcode::IteratorOpen>();
            return operandIndex == OperandIndex::IteratorOpen::iterable && open.isDestructuring() && !open.hasRest();
        }
        return opcode == Opcode::Call && calleeOnlyTakesApart(user, operandIndex);
    }

    bool calleeOnlyTakesApart(const BodyValue& call, uint32_t operandIndex);

    const Walker& m_walker;
    FunctionID m_function;
    const BodyIndex& m_index;
    uint32_t m_callsFollowed;
    Vector<uint32_t> m_rootsOfValues;
    Vector<FateOfValue> m_fates;
};

struct FatesOfCallResults final : Walker::KeptForBuild {
    template<typename Kept> using ByNumber = HashMap<uint32_t, Kept, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;
    ByNumber<ByNumber<FateOfValue>> byFunction;
};

} // anonymous namespace

static bool isParameterOnlyTakenApart(const Walker& walker, FunctionID callee, unsigned entryParameter, uint32_t callsFollowed)
{
    if (callsFollowed >= mostCallsFollowed)
        return false;
    const ThinSummary& summary = walker.facts().summaries().at(callee);
    const BodyIndex* body = walker.facts().bodies().find(callee);
    if (!body || summary.has(SummaryTrait::HasLoops) || summary.numberOfOperations > mostOperationsTakenApart)
        return false;
    const Function& function = walker.program().at(callee);
    BodyBlock entry = body->block(entryBlock);
    if (entryParameter >= entry.numberOfParameters() || entry.numberOfParameters() != function.numberOfEntryParameters())
        return false;
    EntryParameter kind = function.entryParameterKind(entryParameter);
    if ((kind != EntryParameter::Parameter && kind != EntryParameter::This) || function.has(FunctionFlags::HasHiddenRest) || body->holdsAnyOf(opcodesOfMappedArguments))
        return false;
    FatesOfValuesInBody fates(walker, callee, *body, callsFollowed);
    uint32_t root = fates.addRoot(entry.parameter(entryParameter));
    fates.follow([](const BodyValue&) { });
    return fates.fateOf(root) == FateOfValue::OnlyTakenApart;
}

bool FatesOfValuesInBody::calleeOnlyTakesApart(const BodyValue& call, uint32_t operandIndex)
{
    uint32_t numberOfNamedOperands = numberOfNamedOperandsOf(Opcode::Call);
    if (operandIndex < numberOfNamedOperands && operandIndex != OperandIndex::Call::thisValue)
        return false;
    FunctionID callee = m_walker.facts().linked().calleeAt(m_function, call.id());
    if (!callee || callee == m_function)
        return false;
    const Function& function = m_walker.program().at(callee);
    if (operandIndex == OperandIndex::Call::thisValue) {
        std::optional<unsigned> parameter = function.thisParameterIndex();
        return !parameter || isParameterOnlyTakenApart(m_walker, callee, *parameter, m_callsFollowed + 1);
    }
    unsigned position = function.firstParameterIndex() + operandIndex - numberOfNamedOperands;
    if (position >= function.numberOfEntryParameters())
        return !function.hasAny({ FunctionFlags::HasRestParameter, FunctionFlags::HasHiddenRest });
    return isParameterOnlyTakenApart(m_walker, callee, position, m_callsFollowed + 1);
}

bool isParameterOnlyTakenApart(const Walker& walker, FunctionID callee, unsigned entryParameter)
{
    return isParameterOnlyTakenApart(walker, callee, entryParameter, 0);
}

FateOfValue fateOfCallResult(const Walker& walker, FunctionID function, const BodyIndex& index, ValueID call)
{
    std::unique_ptr<Walker::KeptForBuild>& kept = walker.keptByScalarReplacement();
    if (!kept)
        kept = makeUniqueWithoutFastMallocCheck<FatesOfCallResults>();
    auto added = static_cast<FatesOfCallResults&>(*kept).byFunction.add(function.index(), FatesOfCallResults::ByNumber<FateOfValue>());
    if (added.isNewEntry) {
        FatesOfValuesInBody fates(walker, function, index, 0);
        Vector<std::pair<ValueID, uint32_t>> calls;
        fates.follow([&](const BodyValue& operation) {
            if (operation.opcode() == Opcode::Call && !index.isUnused(operation.id()))
                calls.append({ operation.id(), fates.addRoot(operation.id()) });
        });
        for (auto [site, root] : calls)
            added.iterator->value.set(site.index(), fates.fateOf(root));
    }
    auto found = added.iterator->value.find(call.index());
    return found == added.iterator->value.end() ? FateOfValue::Escapes : found->value;
}

static bool returnsNewObject(const Walker& walker, FunctionID callee, uint32_t callsFollowed)
{
    const ThinSummary& summary = walker.facts().summaries().at(callee);
    const BodyIndex* body = walker.facts().bodies().find(callee);
    if (!body || summary.has(SummaryTrait::HasLoops) || summary.numberOfOperations > mostOperationsTakenApart || callsFollowed >= mostCallsFollowed)
        return false;
    bool returnsSome = false;
    for (uint32_t number = 0; number < body->numberOfBlocks(); ++number) {
        if (!body->isReachable(BlockID(number)))
            continue;
        BodyTerminator end = body->terminator(BlockID(number));
        if (end.kind() != TerminatorKind::Return && end.kind() != TerminatorKind::ReturnCall)
            continue;
        ValueID returned = end.operand(0) ? body->stripCasts(end.operand(0)) : ValueID();
        if (!returned)
            return false;
        if (body->isParameter(returned))
            return false;
        static constexpr OpcodeSet allocations { Opcode::NewObject, Opcode::NewArray, Opcode::Construct };
        Opcode opcode = body->opcodeOf(returned);
        if (opcode == Opcode::Null || opcode == Opcode::Undefined)
            continue;
        bool isNew = allocations.contains(opcode);
        if (!isNew && opcode == Opcode::Call) {
            FunctionID called = walker.facts().linked().calleeAt(callee, returned);
            isNew = called && called != callee && returnsNewObject(walker, called, callsFollowed + 1);
        }
        if (!isNew)
            return false;
        returnsSome = true;
    }
    return returnsSome;
}

bool holdsNumbersAlone(const Walker& walker, LayoutID layout)
{
    if (!layout)
        return false;
    const PlannedLayout& planned = walker.plan().layout(layout);
    if (!planned.holdsCompiledObjects() || !planned.numberOfFields())
        return false;
    for (unsigned field = 0; field < planned.numberOfFields(); ++field) {
        const PlacedSlot& slot = planned.slots[field];
        if (!slot.hasStorage())
            continue;
        switch (slot.representation) {
        case SlotRepresentation::Double:
        case SlotRepresentation::OptionalDouble:
        case SlotRepresentation::Int32:
        case SlotRepresentation::OptionalInt32:
        case SlotRepresentation::Int64:
        case SlotRepresentation::OptionalInt64:
            break;
        default:
            return false;
        }
    }
    return true;
}

bool returnsNewObject(const Walker& walker, FunctionID callee)
{
    return returnsNewObject(walker, callee, 0);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
