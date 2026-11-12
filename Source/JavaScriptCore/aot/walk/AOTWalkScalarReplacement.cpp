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

static constexpr uint32_t mostOperationsTakenApart = 48;
static constexpr uint32_t mostCallsFollowed = 3;
static constexpr uint32_t maxRemovableOperations = 80;
static constexpr uint32_t maxRemovableOperationsOfArms = 160;
static constexpr size_t maxRemovableArms = 3;

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
        if (uint32_t known = rootOf(value); known != UINT32_MAX)
            return known;
        uint32_t root = static_cast<uint32_t>(m_fates.size());
        m_rootsOfValues[value.index()] = root;
        m_fates.append(FateOfValue::OnlyTakenApart);
        m_unitedWith.append(root);
        m_areCarriedRoundLoops.append(false);
        return root;
    }
    FateOfValue fateOf(uint32_t root) const { return m_fates[unitedRoot(root)]; }
    bool isCarriedRoundLoop(uint32_t root) const { return m_areCarriedRoundLoops[unitedRoot(root)]; }

    template<typename Functor>
    void follow(const Functor& noteOperation)
    {
        for (uint32_t number = 0; number < m_index.numberOfBlocks(); ++number) {
            BodyBlock block = m_index.block(BlockID(number));
            letEscape(block.subject());
            for (uint32_t position = 0; m_index.isLoopHead(block.id()) && position < block.numberOfParameters(); ++position)
                addRoot(block.parameter(position));
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
                if (root != UINT32_MAX)
                    m_fates[root] = m_fates[root] == FateOfValue::Escapes ? FateOfValue::Escapes : end.kind() == TerminatorKind::Return || end.kind() == TerminatorKind::ReturnCall ? FateOfValue::Returned : FateOfValue::Escapes;
            });
            block.forEachOutgoingEdge([&](BodyTarget target, bool isHandler) {
                std::span<const ValueID> arguments = target.arguments();
                for (uint32_t position = 0; position < arguments.size(); ++position) {
                    if (isHandler || position >= m_index.block(target.block()).numberOfParameters())
                        letEscape(arguments[position]);
                    else
                        passTo(m_index.block(target.block()).parameter(position), arguments[position], target.block().index() <= number);
                }
            });
            m_index.forEachEdgeResult(block.id(), [&](const BodyEdgeResult& result) {
                if (uint32_t root = rootOf(result.subject); root != UINT32_MAX)
                    m_rootsOfValues[result.parameter.index()] = root;
            });
        }
    }

private:
    uint32_t unitedRoot(uint32_t root) const
    {
        while (m_unitedWith[root] != root)
            root = m_unitedWith[root];
        return root;
    }
    uint32_t rootOf(ValueID value) const
    {
        uint32_t root = value ? m_rootsOfValues[value.index()] : UINT32_MAX;
        return root == UINT32_MAX ? root : unitedRoot(root);
    }

    void unite(uint32_t one, uint32_t other)
    {
        if (one == other)
            return;
        m_unitedWith[other] = one;
        m_fates[one] = std::min(m_fates[one], m_fates[other]);
        m_areCarriedRoundLoops[one] |= m_areCarriedRoundLoops[other];
    }

    void passTo(ValueID parameter, ValueID argument, bool isBackEdge)
    {
        uint32_t root = rootOf(argument);
        if (root == UINT32_MAX)
            return;
        uint32_t rootOfParameter = rootOf(parameter);
        if (rootOfParameter == UINT32_MAX) {
            if (isBackEdge)
                m_fates[root] = FateOfValue::Escapes;
            else
                m_rootsOfValues[parameter.index()] = root;
            return;
        }
        m_areCarriedRoundLoops[root] |= isBackEdge;
        unite(root, rootOfParameter);
    }

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
        if (opcode == Opcode::PrivateGet || opcode == Opcode::PrivateSet)
            return !operandIndex;
        if (opcode == Opcode::ElemGet) {
            ValueID key = m_index.stripCasts(user.propertyOperand());
            return operandIndex == OperandIndex::ElemGet::object && !m_index.isParameter(key) && m_index.opcodeOf(key) == Opcode::Integer;
        }
        if (opcode == Opcode::IteratorOpen) {
            auto open = user.as<Opcode::IteratorOpen>();
            return operandIndex == OperandIndex::IteratorOpen::iterable && open.isDestructuring() && !open.hasRest();
        }
        FateOfValue fate = opcode == Opcode::Call ? fateInCallee(user, operandIndex) : FateOfValue::Escapes;
        if (fate != FateOfValue::Returned)
            return fate == FateOfValue::OnlyTakenApart;
        uint32_t root = rootOf(user.operand(operandIndex));
        if (uint32_t rootOfResult = rootOf(user.id()); rootOfResult != UINT32_MAX)
            unite(root, rootOfResult);
        else
            m_rootsOfValues[user.id().index()] = root;
        return true;
    }

    FateOfValue fateInCallee(const BodyValue& call, uint32_t operandIndex);

    const Walker& m_walker;
    FunctionID m_function;
    const BodyIndex& m_index;
    uint32_t m_callsFollowed;
    Vector<uint32_t> m_rootsOfValues;
    Vector<uint32_t> m_unitedWith;
    Vector<bool> m_areCarriedRoundLoops;
    Vector<FateOfValue> m_fates;
};

struct FatesOfCallResults final : Walker::KeptForBuild {
    template<typename Kept> using ByNumber = HashMap<uint32_t, Kept, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;
    ByNumber<ByNumber<FateOfValue>> byFunction;
    ByNumber<bool> returnsUnsharedList;
};

} // anonymous namespace

static FateOfValue fateOfParameter(const Walker& walker, FunctionID callee, unsigned entryParameter, uint32_t callsFollowed)
{
    if (callsFollowed >= mostCallsFollowed)
        return FateOfValue::Escapes;
    const ThinSummary& summary = walker.facts().summaries().at(callee);
    const BodyIndex* body = walker.facts().bodies().find(callee);
    if (!body || summary.has(SummaryTrait::HasLoops) || summary.numberOfOperations > mostOperationsTakenApart)
        return FateOfValue::Escapes;
    const Function& function = walker.program().at(callee);
    BodyBlock entry = body->block(entryBlock);
    if (entryParameter >= entry.numberOfParameters() || entry.numberOfParameters() != function.numberOfEntryParameters())
        return FateOfValue::Escapes;
    EntryParameter kind = function.entryParameterKind(entryParameter);
    if ((kind != EntryParameter::Parameter && kind != EntryParameter::This) || function.has(FunctionFlags::HasHiddenRest) || body->holdsAnyOf(opcodesOfMappedArguments))
        return FateOfValue::Escapes;
    FatesOfValuesInBody fates(walker, callee, *body, callsFollowed);
    uint32_t root = fates.addRoot(entry.parameter(entryParameter));
    fates.follow([](const BodyValue&) { });
    return fates.fateOf(root);
}

FateOfValue FatesOfValuesInBody::fateInCallee(const BodyValue& call, uint32_t operandIndex)
{
    uint32_t numberOfNamedOperands = numberOfNamedOperandsOf(Opcode::Call);
    if (operandIndex < numberOfNamedOperands && operandIndex != OperandIndex::Call::thisValue)
        return FateOfValue::Escapes;
    FunctionID callee = m_walker.facts().linked().calleeAt(m_function, call.id());
    if (!callee || callee == m_function)
        return FateOfValue::Escapes;
    const Function& function = m_walker.program().at(callee);
    if (operandIndex == OperandIndex::Call::thisValue) {
        std::optional<unsigned> parameter = function.thisParameterIndex();
        return parameter ? fateOfParameter(m_walker, callee, *parameter, m_callsFollowed + 1) : FateOfValue::OnlyTakenApart;
    }
    unsigned position = function.firstParameterIndex() + operandIndex - numberOfNamedOperands;
    if (position >= function.numberOfEntryParameters())
        return function.hasAny({ FunctionFlags::HasRestParameter, FunctionFlags::HasHiddenRest }) ? FateOfValue::Escapes : FateOfValue::OnlyTakenApart;
    return fateOfParameter(m_walker, callee, position, m_callsFollowed + 1);
}

FateOfValue fateOfParameter(const Walker& walker, FunctionID callee, unsigned entryParameter)
{
    return fateOfParameter(walker, callee, entryParameter, 0);
}

static FatesOfCallResults& fatesKeptForBuild(const Walker& walker)
{
    std::unique_ptr<Walker::KeptForBuild>& kept = walker.keptByScalarReplacement();
    if (!kept)
        kept = makeUniqueWithoutFastMallocCheck<FatesOfCallResults>();
    return static_cast<FatesOfCallResults&>(*kept);
}

FateOfValue fateOfCallResult(const Walker& walker, FunctionID function, const BodyIndex& index, ValueID call)
{
    auto added = fatesKeptForBuild(walker).byFunction.add(function.index(), FatesOfCallResults::ByNumber<FateOfValue>());
    if (added.isNewEntry) {
        FatesOfValuesInBody fates(walker, function, index, 0);
        Vector<std::pair<ValueID, uint32_t>> calls;
        fates.follow([&](const BodyValue& operation) {
            if ((operation.opcode() == Opcode::Call || operation.opcode() == Opcode::Construct) && !index.isUnused(operation.id()))
                calls.append({ operation.id(), fates.addRoot(operation.id()) });
        });
        for (auto [site, root] : calls) {
            FateOfValue fate = fates.fateOf(root);
            added.iterator->value.set(site.index(), fate == FateOfValue::Returned && fates.isCarriedRoundLoop(root) ? FateOfValue::OnlyTakenApart : fate);
        }
    }
    auto found = added.iterator->value.find(call.index());
    return found == added.iterator->value.end() ? FateOfValue::Escapes : found->value;
}

static ValueID objectReturnedByConstructor(const BodyIndex& body, ValueID returned)
{
    BlockID end = body.blockOf(returned);
    uint32_t position = returned.index() - body.block(end).firstParameter().index();
    Vector<BlockID, 2> neverEntered;
    ValueID found;
    for (uint32_t number = 0; number < body.numberOfBlocks(); ++number) {
        BodyTerminator terminator = body.terminator(BlockID(number));
        ValueID condition = terminator.isConditionalBranch() ? body.stripCasts(terminator.operand(0)) : ValueID();
        if (condition && !body.isParameter(condition) && body.opcodeOf(condition) == Opcode::TypeTest) {
            ValueID tested = body.stripCasts(body.value(condition).operand(0));
            if (tested && !body.isParameter(tested) && body.opcodeOf(tested) == Opcode::Undefined && body.value(condition).as<Opcode::TypeTest>().testedType() == TypeTable::object)
                neverEntered.append(terminator.taken().block());
        }
        if (neverEntered.contains(BlockID(number)))
            continue;
        for (BodyTarget target : terminator.targets()) {
            if (target.block() != end)
                continue;
            ValueID passed = position < target.arguments().size() && target.arguments()[position] ? body.stripCasts(target.arguments()[position]) : ValueID();
            if (!passed || (found && passed != found))
                return { };
            found = passed;
        }
    }
    return found;
}

static bool returnsNewObject(const Walker& walker, FunctionID callee, uint32_t callsFollowed, uint32_t maxOperations = mostOperationsTakenApart)
{
    const ThinSummary& summary = walker.facts().summaries().at(callee);
    const BodyIndex* body = walker.facts().bodies().find(callee);
    if (!body || summary.has(SummaryTrait::HasLoops) || summary.numberOfOperations > maxOperations || callsFollowed >= mostCallsFollowed)
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
        if (body->isParameter(returned) && walker.program().at(callee).kind == FunctionKind::BaseConstructor)
            returned = objectReturnedByConstructor(*body, returned);
        if (!returned || body->isParameter(returned))
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
    const Vector<Field>& fields = walker.program().at(layout).fields;
    constexpr OptionSet<FieldFlags> brands { FieldFlags::IsBrand, FieldFlags::IsBrandOfMethods };
    for (unsigned field = 0; field < planned.numberOfFields(); ++field) {
        const PlacedSlot& slot = planned.slots[field];
        if (!slot.hasStorage() || (field < fields.size() && fields[field].flags.containsAny(brands)))
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

namespace {

class ReturnedLists {
public:
    ReturnedLists(const Program& program, const BodyIndex& index)
        : m_program(program)
        , m_index(index)
    {
        m_roles.fill(Role::Other, index.numberOfValues());
    }

    bool areUnshared()
    {
        bool returnsSome = false;
        for (uint32_t number = 0; number < m_index.numberOfBlocks(); ++number) {
            if (!m_index.isReachable(BlockID(number)))
                continue;
            BodyTerminator end = m_index.terminator(BlockID(number));
            if (end.kind() == TerminatorKind::ReturnCall)
                return false;
            if (end.kind() != TerminatorKind::Return)
                continue;
            ValueID returned = end.operand(0) ? m_index.stripCasts(end.operand(0)) : ValueID();
            if (!returned || m_index.isParameter(returned) || !isEmptyLiteral(m_index.value(returned)))
                return false;
            m_roles[returned.index()] = Role::List;
            returnsSome = true;
        }
        if (!returnsSome)
            return false;

        for (uint32_t number = 0; number < m_index.numberOfBlocks(); ++number) {
            BodyBlock block = m_index.block(BlockID(number));
            if (roleOf(block.subject()) == Role::List)
                return false;
            for (BodyValue user : block.values()) {
                bool staysUnshared = true;
                user.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                    if (roleOf(operand) == Role::List)
                        staysUnshared &= leavesListUnshared(user, operandIndex);
                });
                if (!staysUnshared)
                    return false;
            }
            BodyTerminator end = block.terminator();
            bool endsWithList = false;
            end.forEachOperand([&](ValueID operand, uint32_t) {
                endsWithList |= roleOf(operand) == Role::List;
            });
            if (endsWithList && end.kind() != TerminatorKind::Return)
                return false;
            bool handsListOn = false;
            block.forEachOutgoingEdge([&](BodyTarget target, bool) {
                for (ValueID argument : target.arguments())
                    handsListOn |= roleOf(argument) == Role::List;
            });
            if (handsListOn)
                return false;
        }
        return true;
    }

private:
    enum class Role : uint8_t { Other, List, Push };

    Role roleOf(ValueID value) const { return value ? m_roles[value.index()] : Role::Other; }

    bool isEmptyLiteral(const BodyValue& made) const
    {
        if (made.opcode() == Opcode::NewArray)
            return made.as<Opcode::NewArray>().elementsOperands().empty() && made.as<Opcode::NewArray>().holes().empty();
        if (made.opcode() != Opcode::Data)
            return false;
        DataID data = made.as<Opcode::Data>().data();
        const auto* literal = data ? m_program.at(data).root.tryAs<DataNode::Array>() : nullptr;
        return literal && literal->elements.isEmpty();
    }

    bool leavesListUnshared(const BodyValue& user, uint32_t operandIndex)
    {
        Opcode opcode = user.opcode();
        if (inputPassThroughOpcodes.contains(opcode)) {
            m_roles[user.id().index()] = Role::List;
            return !operandIndex;
        }
        if (opcode == Opcode::PropGet) {
            if (operandIndex != OperandIndex::PropGet::object || user.isFreeName())
                return false;
            if (user.key() == m_program.keyOf(BuiltinNames::length))
                return true;
            m_roles[user.id().index()] = Role::Push;
            return user.key() == m_program.keyOf(BuiltinNames::push);
        }
        if (opcode != Opcode::Call || operandIndex != OperandIndex::Call::thisValue)
            return false;
        auto call = user.as<Opcode::Call>();
        ValueID callee = call.calleeOperand() ? m_index.stripCasts(call.calleeOperand()) : ValueID();
        return roleOf(callee) == Role::Push;
    }

    const Program& m_program;
    const BodyIndex& m_index;
    Vector<Role> m_roles;
};

} // anonymous namespace

bool returnsUnsharedList(const Walker& walker, FunctionID callee)
{
    const Program& program = walker.program();
    const Function& function = program.at(callee);
    if (program.admitsReplacedBuiltins || function.isCoroutine() || function.isModuleBody())
        return false;
    const BodyIndex* body = walker.facts().bodies().find(callee);
    if (!body || !body->holdsAnyOf(OpcodeSet { Opcode::Data, Opcode::NewArray }))
        return false;
    auto added = fatesKeptForBuild(walker).returnsUnsharedList.add(callee.index(), false);
    if (added.isNewEntry)
        added.iterator->value = ReturnedLists(program, *body).areUnshared();
    return added.iterator->value;
}

bool isRemovableInLoops(const Walker& walker, FunctionID callee)
{
    return returnsNewObject(walker, callee, 0, maxRemovableOperations);
}

bool armsRemoveObjects(const Walker& walker, std::span<const FunctionID> methods, const BodyIndex& index, ValueID call)
{
    if (methods.size() > maxRemovableArms)
        return false;
    uint32_t numberOfOperations = 0;
    for (FunctionID method : methods) {
        if (walker.facts().linked().function(method).neverReturns)
            continue;
        if (!isRemovableInLoops(walker, method))
            return false;
        numberOfOperations += walker.facts().summaries().at(method).numberOfOperations;
    }
    return numberOfOperations && numberOfOperations <= maxRemovableOperationsOfArms && fateOfCallResult(walker, walker.currentFunction(), index, call) != FateOfValue::Escapes;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
