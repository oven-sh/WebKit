/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"
#include "AOTWalkNumberGuesses.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTCodeRole.h"
#include "AOTEffectsDeclared.h"
#include "AOTFacts.h"
#include "AOTIRProgram.h"
#include "AOTOpcodeSets.h"
#include "AOTPlan.h"
#include "AOTRepresentationRules.h"
#include "AOTSchemaOperandIndexes.h"
#include "AOTSweep.h"
#include "AOTThinSummary.h"
#include "AOTWalkKnown.h"
#include <algorithm>
#include <wtf/DataLog.h>
#include <wtf/SetForScope.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OperandUses usesThatLeaveCellToItsFramesInEnvironmentCells { OperandUse::Inspects, OperandUse::Returns };

static_assert(resultRuleOf(Opcode::MarkEnvironmentEscaped) == ResultRule::None && resultRuleOf(Opcode::ReleaseEnvironment) == ResultRule::None);
static_assert(!mayThrow(Opcode::MarkEnvironmentEscaped) && !mayThrow(Opcode::ReleaseEnvironment) && !maySuspend(Opcode::MarkEnvironmentEscaped) && !maySuspend(Opcode::ReleaseEnvironment));

static bool kindAdmitsEnvironmentInEnvironmentCells(OperandKind kind)
{
    return kind == OperandKind::Layout || kind == OperandKind::Declared || kind == OperandKind::Results || kind == OperandKind::Environment || kind == OperandKind::EvalOperand || kind == OperandKind::Convertible;
}

static bool looksAtUsesOfOperandsInEnvironmentCells(Opcode opcode)
{
    return !maySuspend(opcode) && opcode != Opcode::Results;
}

OpcodeSet operationsThatMayKeepEnvironments()
{
    return opcodesWhere([](Opcode opcode) {
        if (opcode == Opcode::CallDirect)
            return false;
        if (opcode == Opcode::NewObject || opcode == Opcode::ResultAt)
            return true;
        uint32_t numberOfDeclaredOperands = numberOfNamedOperandsOf(opcode) + hasVariadicOperand(opcode);
        for (uint32_t operandIndex = 0; operandIndex < numberOfDeclaredOperands; ++operandIndex) {
            const OperandInfo& info = operandInfoOf(opcode, operandIndex);
            if (!kindAdmitsEnvironmentInEnvironmentCells(info.kind))
                continue;
            if (!looksAtUsesOfOperandsInEnvironmentCells(opcode) || !info.protocols.isEmpty() || !info.uses.containsOnly(usesThatLeaveCellToItsFramesInEnvironmentCells))
                return true;
        }
        return false;
    });
}

static bool cellsAreHandedBackInEnvironmentCells(const Plan& plan, LayoutID layout)
{
    const PlannedLayout& planned = plan.layout(layout);
    return planned.cellsAreHandedBack;
}

static ValueID recordBehindNamesInEnvironmentCells(const Code& code, Reference reference)
{
    if (!reference || !reference.isValue())
        return { };
    ValueID root = underlyingCellOf(code, reference.id());
    return code.isConstant(root) ? ValueID() : root;
}

static LayoutID layoutHandedBackInEnvironmentCells(Walker& walker, ValueID record)
{
    TypeID type = walker.code().at(record).type;
    if (!type || holdsOnlyPrimitives(type))
        return { };
    LayoutID environment = walker.types().at(type).environment;
    return environment && cellsAreHandedBackInEnvironmentCells(walker.plan(), environment) ? environment : LayoutID();
}

static bool mayBeNothingInEnvironmentCells(Walker& walker, ValueID record)
{
    return walker.types().at(walker.code().at(record).type).primitives;
}

static ValueID cellMadeByUnitInEnvironmentCells(Walker& walker, Reference reference)
{
    ValueID root = recordBehindNamesInEnvironmentCells(walker.code(), reference);
    if (!root)
        return { };
    const Operation& made = walker.code().at(root);
    if (made.opcode != Opcode::NewObject || made.storage() != AllocationStorage::Heap || !cellsAreHandedBackInEnvironmentCells(walker.plan(), made.layout()))
        return { };
    return root;
}

static bool buildMarkInEnvironmentCells(Walker& walker, Reference environment, Origin origin)
{
    origin.isTailCall = false;
    Draft mark(Opcode::MarkEnvironmentEscaped, origin);
    mark.setEnvironmentOperand(environment);
    walker.build(WTF::move(mark));
    return !walker.isUnreachable();
}

bool isHandedEnvironmentWithByte(const Walker& walker, FunctionID function)
{
    for (const DeclaredCapture& capture : walker.program().at(function).captures) {
        TypeID written = capture.written();
        LayoutID environment = written && !holdsOnlyPrimitives(written) ? walker.program().at(written).environment : LayoutID();
        if (environment && cellsAreHandedBackInEnvironmentCells(walker.plan(), environment))
            return true;
    }
    return false;
}

void markEnvironmentsThatOperationKeeps(Walker& walker, const Draft& draft)
{
    Opcode opcode = draft.opcode;
    if (opcode == Opcode::ResultAt)
        return;
    if (opcode == Opcode::NewClosure && draft.closureForm() == ClosureForm::Deferred)
        return;

    bool looksAtUsesOfOperands = looksAtUsesOfOperandsInEnvironmentCells(opcode);
    uint32_t indexOfList = numberOfNamedOperandsOf(opcode);
    std::span<const Reference> operands = draft.operands();
    for (uint32_t operandIndex = 0; operandIndex < operands.size(); ++operandIndex) {
        Reference operand = operands[operandIndex];
        if (!operand || !operand.isValue())
            continue;
        if (looksAtUsesOfOperands) {
            const OperandInfo& info = operandInfoOf(opcode, std::min(operandIndex, indexOfList));
            if (info.protocols.isEmpty() && info.uses.containsOnly(usesThatLeaveCellToItsFramesInEnvironmentCells))
                continue;
        }
        if (!layoutHandedBackInEnvironmentCells(walker, operand.id()))
            continue;
        if (maySuspend(opcode) && cellMadeByUnitInEnvironmentCells(walker, operand))
            continue;
        if (opcode == Opcode::Results && cellMadeByUnitInEnvironmentCells(walker, operand))
            continue;
        if (mayBeNothingInEnvironmentCells(walker, operand.id())) {
            walker.fail("what may be no environment is kept, and nobody can tell whether it has a byte to set"_s);
            return;
        }
        if (opcode == Opcode::NewClosure)
            continue;
        if (!buildMarkInEnvironmentCells(walker, operand, draft.origin()))
            return;
    }
}

static void markWhatCoroutineIsHandedInEnvironmentCells(Walker& walker)
{
    BodyBlock entry = walker.currentBodyIndex().block(entryBlock);
    uint32_t numberOfCaptures = static_cast<uint32_t>(walker.program().at(walker.unit().function).captures.size());
    for (uint32_t capture = 0; capture < numberOfCaptures && capture < entry.numberOfParameters(); ++capture) {
        Reference capturedValue = walker.referenceBoundTo(entry.parameter(capture));
        if (!capturedValue || !capturedValue.isValue() || !layoutHandedBackInEnvironmentCells(walker, capturedValue.id()))
            continue;
        if (mayBeNothingInEnvironmentCells(walker, capturedValue.id())) {
            walker.fail("what may be no environment is kept, and nobody can tell whether it has a byte to set"_s);
            return;
        }
        if (!buildMarkInEnvironmentCells(walker, capturedValue, walker.originOfOperation()))
            return;
    }
}

void noteEnvironmentThatUnitMakes(Walker& walker, const Draft& allocation)
{
    Walker::EnvironmentCellsOfUnit& cells = walker.environmentCellsOfUnit();
    cells.hasMadeSome = true;
    cells.mayNameSome = true;
    ValueID name = allocation.inputValueGiven;
    if (!name || allocation.standsForNoValueOfInput)
        return;
    const BodyIndex& input = walker.currentBodyIndex();
    BlockID making = input.blockOf(name);
    if (input.loopOf(making) || input.block(making).hasHandler() || input.hasIrreducibleFlow() || walker.program().at(walker.currentFunction()).isCoroutine())
        return;
    Vector<Walker::EnvironmentMadeInFrame, 2>& made = walker.environmentsMadeInFrame();
    size_t place = std::ranges::lower_bound(made, name, { }, &Walker::EnvironmentMadeInFrame::name) - made.begin();
    if (place < made.size() && made[place].name == name)
        made[place].hasSeveralRecords = true;
    else
        made.insert(place, Walker::EnvironmentMadeInFrame { name, making, false });
}

void handBackEnvironmentsAtReturn(Walker& walker, BlockID block)
{
    if (walker.isUnreachable())
        return;
    const BodyIndex& input = walker.currentBodyIndex();
    Origin origin = walker.originOfOperation();
    origin.isTailCall = false;
    const Vector<Walker::EnvironmentMadeInFrame, 2>& made = walker.environmentsMadeInFrame();
    for (size_t index = 0; index < made.size(); ++index) {
        const Walker::EnvironmentMadeInFrame& name = made[index];
        if (name.hasSeveralRecords || !input.dominates(name.making, block))
            continue;
        ValueID record = cellMadeByUnitInEnvironmentCells(walker, walker.nameLastBoundTo(name.name));
        if (!record)
            continue;
        Draft release(Opcode::ReleaseEnvironment, origin);
        release.setEnvironmentOperand(Reference::value(record));
        walker.build(WTF::move(release));
        if (walker.isUnreachable())
            return;
    }
}

void markEnvironmentsThatCoroutineIsHanded(Walker& walker)
{
    if (!walker.isUnreachable() && isBodyOfCoroutine(walker.unit().role, walker.program().at(walker.unit().function).isCoroutine()))
        markWhatCoroutineIsHandedInEnvironmentCells(walker);
}

bool Walker::isEnvironmentAllocation(Reference cell) const
{
    if (!cell || !cell.isValue())
        return false;
    const Operation& made = m_code.at(cell.id());
    return made.opcode == Opcode::NewObject && program().at(made.layout()).isEnvironment();
}

bool Walker::namesFreshCell(const WalkState& state, Reference value) const
{
    return value && value.isValue() && state.isFresh(referenceTo(underlyingCellOf(m_code, value.id())));
}

void Walker::listScopesThatValueNames(Reference value, Vector<Reference, 2>& scopes) const
{
    if (!value.isValue())
        return;
    Reference cell = referenceTo(underlyingCellOf(m_code, value.id()));
    if (isEnvironmentAllocation(cell))
        scopes.append(cell);
    if (!cell.isValue() || m_code.at(cell.id()).opcode != Opcode::NewClosure)
        return;
    for (ValueID capture : m_code.at(cell.id()).capturesOperands()) {
        Reference held = capture ? referenceTo(underlyingCellOf(m_code, capture)) : Reference();
        if (isEnvironmentAllocation(held))
            scopes.append(held);
    }
}

bool Walker::mayMeetUnmadeRecords(std::span<const uint32_t> edges) const
{
    const WalkState& first = m_openEdges[edges[0]].state;
    if (!mayMeetUnmadeRecords(first))
        return false;
    return std::ranges::any_of(edges.subspan(1), [&](uint32_t place) {
        return m_openEdges[place].state.newestStoreIntoUnmadeRecord() != first.newestStoreIntoUnmadeRecord();
    });
}

Reference Walker::carriedResultOf(const WalkState& state, Reference headerParameter) const
{
    std::optional<uint64_t> noted = findKnown(*this, state, KnownNote::CarriedResult, headerParameter);
    return noted ? Reference::fromBits(static_cast<uint32_t>(*noted)) : headerParameter;
}

void Walker::findCarriedResultsThatMeet(std::span<const WalkState* const> states, Vector<FieldOfScope, 4>& found)
{
    Vector<Representation, 4> forms;
    for (const OpenLoop& loop : m_openLoops) {
        if (!loop.carriedResultCount)
            continue;
        std::span<const ValueID> parameters = m_code.at(loop.head).parameters();
        for (ValueID parameter : parameters.last(std::min<size_t>(loop.carriedResultCount, parameters.size()))) {
            FieldOfScope field;
            field.carriedResult = Reference::value(parameter);
            bool allBringOneValue = true;
            forms.shrink(0);
            for (const WalkState* state : states) {
                Reference value = carriedResultOf(*state, field.carriedResult);
                allBringOneValue &= field.values.isEmpty() || value == field.values[0];
                field.type = field.type ? joinTypes(field.type, ownTypeOf(value)) : ownTypeOf(value);
                field.values.append(value);
                forms.append(representationOf(value));
            }
            if (allBringOneValue)
                continue;
            field.form = formOfParameter(field.type, forms.span());
            found.append(WTF::move(field));
        }
    }
}

void Walker::findFieldsOfScopesThatMeet(std::span<const WalkState* const> states, Vector<FieldOfScope, 4>& found)
{
    if (states.size() < 2)
        return;
    if (!m_carriedResultRequests.isEmpty())
        findCarriedResultsThatMeet(states, found);
    Vector<Representation, 4> forms;
    Vector<const Vector<FieldOfScope>*, 4> fieldsOfOpenLoops;
    for (const OpenLoop& loop : m_openLoops) {
        if (m_fieldsOfScopesOfHeads.isEmpty())
            break;
        if (auto fields = m_fieldsOfScopesOfHeads.find(loop.head.index()); fields != m_fieldsOfScopesOfHeads.end())
            fieldsOfOpenLoops.append(&fields->value);
    }
    bool meetsUnmadeRecords = mayMeetUnmadeRecords(*states[0]) && std::ranges::any_of(states.subspan(1), [&](const WalkState* state) {
        return state->newestStoreIntoUnmadeRecord() != states[0]->newestStoreIntoUnmadeRecord();
    });
    if (fieldsOfOpenLoops.isEmpty() && !unitHasMadeScopes() && !meetsUnmadeRecords)
        return;
    auto isFilledOnEveryWay = [&](Reference holder, FieldReference place) {
        if (!holder.isValue() || m_code.isConstant(holder.id()) || m_code.at(holder.id()).opcode != Opcode::NewObject || m_code.at(holder.id()).layout() != place.layout || isEnvironmentAllocation(holder))
            return false;
        std::optional<uint32_t> orderWord = states[0]->orderWordOf(holder);
        bool isUnmadeInOneOrder = std::ranges::all_of(states, [&](const WalkState* state) {
            return state->isFresh(holder) && state->orderWordOf(holder) == orderWord;
        });
        return (orderWord && isUnmadeInOneOrder && orderWordContainsField(*this, holder, *orderWord, place.field));
    };
    auto findCarriedField = [&](Reference holder, FieldReference place, bool isSlot) -> const FieldOfScope* {
        for (const Vector<FieldOfScope>* fields : fieldsOfOpenLoops) {
            size_t found = fields->findIf([&](const FieldOfScope& field) { return field.scope == holder && !!field.slotKey == isSlot && field.field.layout == place.layout && field.field.field == place.field; });
            if (found != notFound)
                return &fields->at(found);
        }
        return nullptr;
    };
    auto meetInParameter = [&](FieldOfScope&& field, const auto& valueOn, bool takesNothing = false) {
        bool allBringOneValue = true;
        forms.shrink(0);
        for (const WalkState* state : states) {
            Reference value = valueOn(*state);
            TypeID type = value ? ownTypeOf(value) : TypeID();
            if (!type || (m_types.at(type).primitives & Primitive::Absent) || (!takesNothing && representationOf(value) == Representation::Void) || namesFreshCell(*state, value) || isClosureLeftUnmade(*this, value))
                return false;
            allBringOneValue &= field.values.isEmpty() || value == field.values[0];
            field.type = field.type ? joinTypes(field.type, type) : type;
            field.values.append(value);
            forms.append(representationOf(value));
        }
        if (allBringOneValue)
            return false;
        field.form = formOfParameter(field.type, forms.span());
        found.append(WTF::move(field));
        return true;
    };
    for (const Vector<FieldOfScope>* fields : fieldsOfOpenLoops) {
        for (const FieldOfScope& carried : *fields) {
            if (!carried.variable || found.containsIf([&](const FieldOfScope& other) { return other.variable == carried.variable; }))
                continue;
            FieldOfScope field;
            field.variable = carried.variable;
            if (meetInParameter(WTF::move(field), [&](const WalkState& state) { return state.variable(carried.variable); }))
                count(StatisticID::CarriedVariablesMergedAtJoins);
        }
    }
    states[0]->forEachFieldNotSharedWithAll(states, [&](Reference holder, FieldReference place, Reference, bool isSlot) {
        const FieldOfScope* carried = isSlot || !isEnvironmentAllocation(holder) ? findCarriedField(holder, place, isSlot) : nullptr;
        PropertyKeyID keyOfUnmade = !carried && isSlot && meetsUnmadeRecords && isFilledOnEveryWay(holder, place) ? plan().slots().keyOf(program(), place) : PropertyKeyID();
        if (!carried && !keyOfUnmade && (isSlot || !isEnvironmentAllocation(holder)))
            return;
        FieldOfScope field { holder, place, TypeID(), Representation::JSValue, { }, { }, { } };
        if (keyOfUnmade)
            field.slotKey = keyOfUnmade;
        else if (isSlot) {
            field.slotKey = carried->slotKey;
            field.receivers = carried->receivers;
        }
        if (meetInParameter(WTF::move(field), [&](const WalkState& state) { return isSlot ? state.slot(holder, place) : state.environmentField(holder, place); }, !!keyOfUnmade) && isSlot)
            count(StatisticID::CarriedSlotsMergedAtJoins);
    });
}

void Walker::findFieldsOfScopesInFrontOfLoop(const LoopPreview& look, Vector<FieldOfScope, 4>& known)
{
    for (Reference scope : look.scopesThatTurnsOnlyBorrow) {
        for (FieldReference place : std::ranges::equal_range(look.fieldsOfScopesThatTurnsStore, m_code.at(scope.id()).layout(), { }, &FieldReference::layout)) {
            bool mayComeRound = !std::ranges::binary_search(look.fieldsOfScopesThatCallsInTurnsStore, place);
            Reference value = mayComeRound ? m_state.environmentField(scope, place) : Reference();
            if (!value)
                continue;
            known.append(FieldOfScope { scope, place, TypeID(), Representation::JSValue, { }, { value }, { } });
            size_t counter = look.counters.findIf([&](const CapturedCounter& found) { return found.field == place && !found.hasLeftIntegers; });
            if (counter == notFound)
                continue;
            known.last().mayStartAsInteger = true;
            known.last().startingIntegers = look.counters[counter].integers;
        }
    }
}

static constexpr int64_t largestStepOfCounter = 64;

void Walker::noteCapturedCounter(TrialVisit& visit, const FieldOfScope& field, Reference comingRound)
{
    TypeID entering = field.values[0] && field.values[0].isRecord() ? ownTypeOf(field.values[0]) : TypeID();
    if (!entering || !m_types.at(entering).isInteger() || m_types.at(field.type).isInteger())
        return;
    std::optional<Range> steps = comingRound.isValue() && field.parameter.isValue() ? findStepsFrom(m_code, m_types, comingRound.id(), field.parameter.id(), StepsAreExact::AsAssumed) : std::nullopt;
    Range integers = m_types.at(entering).integers;
    bool isCounter = steps && steps->isSubsetOf(Range { -largestStepOfCounter, largestStepOfCounter }) && !integers.isEmpty() && integers.isSubsetOf(Range::exact());
    if (isCounter && steps->minimum < 0)
        integers.minimum = -maxExactInteger;
    if (isCounter && steps->maximum > 0)
        integers.maximum = maxExactInteger;
    size_t place = visit.counters.findIf([&](const CapturedCounter& found) { return found.field == field.field; });
    if (place == notFound) {
        visit.counters.append(CapturedCounter { field.field, integers, field.parameter.isValue() ? field.parameter.id() : ValueID(), !isCounter });
        return;
    }
    CapturedCounter& counter = visit.counters[place];
    counter.hasLeftIntegers |= !isCounter || !field.parameter.isValue() || counter.parameter != field.parameter.id();
    counter.integers = counter.integers.unionWith(integers);
}

void Walker::noteSlotOfHeadParameter(ValueID parameter, PropertyKeyID key)
{
    BlockID block = m_code.at(parameter).block();
    for (TrialVisit& visit : m_trialVisits) {
        if (visit.frameIndex >= m_frames.size() || inputBlock(visit.frameIndex, visit.inputHead).head != block)
            continue;
        std::span<const ValueID> parameters = m_code.at(block).parameters();
        size_t position = std::ranges::find(parameters, parameter) - parameters.begin();
        bool isListed = visit.slotsOfHeadParameters.containsIf([&](const TrialVisit::SlotOfHeadParameter& slot) { return slot.position == position && slot.key == key; });
        if (position < m_frames[visit.frameIndex].index->block(visit.inputHead).numberOfParameters() && !isListed)
            visit.slotsOfHeadParameters.append({ static_cast<uint32_t>(position), key });
    }
}

void Walker::noteSlotOfParameter(ValueID parameter, PropertyKeyID key)
{
    const Block& block = m_code.at(m_code.at(parameter).block());
    if (block.isLoopHead) {
        noteSlotOfHeadParameter(parameter, key);
        return;
    }
    size_t position = std::ranges::find(block.parameters(), parameter) - block.parameters().begin();
    for (EdgeID edge = block.firstEdgeIn(); edge; edge = m_code.at(edge).nextInto()) {
        std::span<const ValueID> arguments = m_code.at(edge).arguments();
        ValueID handedOn = position < arguments.size() ? arguments[position] : ValueID();
        if (!handedOn || m_code.isConstant(handedOn))
            continue;
        ValueID holder = underlyingCellOf(m_code, handedOn);
        if (!m_code.isConstant(holder) && m_code.at(holder).opcode == Opcode::Parameter && m_code.at(m_code.at(holder).block()).isLoopHead)
            noteSlotOfHeadParameter(holder, key);
    }
}

void Walker::findSlotsOfObjectsThatMeet(std::span<const uint32_t> edges, std::span<const uint32_t> joined, std::span<const uint32_t> joinedNames, std::span<const ValueID> parameters, std::span<const ObjectsMetAtJoin> objectsMet, Vector<FieldOfScope, 4>& found)
{
    Vector<PropertyKeyID, 4> keys;
    for (const TrialVisit& visit : m_trialVisits) {
        for (const TrialVisit::SlotOfHeadParameter& slot : visit.slotsOfHeadParameters) {
            if (!keys.contains(slot.key))
                keys.append(slot.key);
        }
    }
    if (m_carriesSlotsOfObjects) {
        for (uint32_t head : m_blocksWithFieldsOfScopesInOrder) {
            for (const FieldOfScope& field : m_fieldsOfScopesOfHeads.find(head)->value) {
                if (field.positionOfHolder && !keys.contains(field.slotKey))
                    keys.append(field.slotKey);
            }
        }
    }
    if (keys.isEmpty())
        return;
    Vector<Representation, 4> forms;
    auto objectOfWay = [&](uint32_t edge, size_t place) {
        return place < joined.size() ? m_openEdges[edge].arguments[joined[place]] : m_openEdges[edge].names.nameIn(joinedNames[place - joined.size()]);
    };
    for (size_t place = 0; place < joined.size() + joinedNames.size(); ++place) {
        if (std::ranges::any_of(objectsMet, [&](const ObjectsMetAtJoin& met) { return met.placeAmongParameters == place; }))
            continue;
        Reference parameter = Reference::value(parameters[place]);
        for (PropertyKeyID key : keys) {
            std::optional<FilledSlot> filled = findFilledSlot(*this, typeOf(parameter), key);
            if (!filled)
                continue;
            FieldOfScope field;
            field.scope = parameter;
            field.field = filled->place;
            field.slotKey = key;
            field.receivers = filled->receivers;
            field.positionOfHolder = static_cast<uint32_t>(place);
            forms.shrink(0);
            bool isKnownOnUsualWays = true;
            bool isKnownOnSomePath = false;
            for (uint32_t edge : edges) {
                Reference object = objectOfWay(edge, place);
                const WalkState& state = m_openEdges[edge].state;
                Reference value = object && object.isValue() ? state.slot(object, filled->place) : Reference();
                TypeID type = value ? ownTypeOf(value) : TypeID();
                if (!type || (m_types.at(type).primitives & Primitive::Absent) || representationOf(value) == Representation::Void || namesFreshCell(state, value) || isClosureLeftUnmade(*this, value)) {
                    value = { };
                    type = resultTypeOf(draftVariableRead(field));
                    isKnownOnUsualWays &= object && object.isValue() && type && m_types.at(type).isJavaScriptValue() && m_code.at(m_code.at(m_openEdges[edge].edge).from).frequency == BlockFrequency::Rare;
                }
                if (!isKnownOnUsualWays)
                    break;
                isKnownOnSomePath |= !!value;
                field.type = field.type ? joinTypes(field.type, type) : type;
                field.values.append(value);
                if (value)
                    forms.append(representationOf(value));
            }
            if (!isKnownOnUsualWays || !isKnownOnSomePath)
                continue;
            field.form = formOfParameter(field.type, forms.span());
            found.append(WTF::move(field));
        }
    }
}

Reference Walker::readSlotOnEdge(EdgeID& edge, BlockID to, const FieldOfScope& field, Reference object)
{
    if (edge) {
        layBlockOnEdge(edge, to);
        m_code.makeBlockRare(m_block);
    }
    Draft read = draftVariableRead(field);
    read.setObjectOperand(object);
    return build(WTF::move(read));
}

void Walker::noteSlotsThatComeRound(TrialVisit& visit, std::span<const Reference> arguments)
{
    for (TrialVisit::SlotOfHeadParameter& slot : visit.slotsOfHeadParameters) {
        Reference object = slot.position < arguments.size() ? arguments[slot.position] : Reference();
        if (!object || !object.isValue())
            continue;
        Reference root = referenceTo(underlyingCellOf(m_code, object.id()));
        if (!root.isValue() || (!m_state.isFresh(root) && m_code.at(root.id()).opcode != Opcode::Parameter))
            continue;
        std::optional<FilledSlot> filled = findFilledSlot(*this, typeOf(object), slot.key);
        Reference value = filled ? m_state.slot(object, filled->place) : Reference();
        slot.numberOfEdgesThatKnowIt += value && ownTypeOf(value) && !namesFreshCell(m_state, value) && !isClosureLeftUnmade(*this, value);
    }
}

bool Walker::learnSlotsOfObjectsThatComeRound(LoopSummary& summary, const TrialVisit& visit)
{
    const BodyIndex& index = *m_frames[visit.frameIndex].index;
    BodyBlock head = index.block(visit.inputHead);
    Vector<SlotOfLoop, 2>& slots = summary.storesIntoScopes.slots;
    auto learn = [&](ValueID holder, PropertyKeyID key) {
        auto place = std::ranges::lower_bound(slots, std::pair { holder, key.bits() }, { }, &SlotOfLoop::order);
        if (place != slots.end() && place->holder == holder && place->key == key)
            return false;
        slots.insert(place - slots.begin(), SlotOfLoop { holder, key });
        return true;
    };
    Vector<ValueID, 2> holders;
    for (const TrialVisit::SlotOfHeadParameter& slot : visit.slotsOfHeadParameters) {
        if (!slot.numberOfEdgesThatKnowIt || slot.numberOfEdgesThatKnowIt != visit.numberOfEdgesThatCameRound)
            continue;
        ValueID holder = head.parameter(slot.position);
        if (learn(holder, slot.key) && !holders.contains(holder))
            holders.append(holder);
    }
    if (holders.isEmpty())
        return false;
    LoopID loop = index.loopOf(visit.inputHead);
    for (uint32_t number = 0; number < index.numberOfBlocks(); ++number) {
        if (index.isInLoop(BlockID(number), loop))
            continue;
        for (BodyValue read : index.block(BlockID(number)).values()) {
            if (read.opcode() != Opcode::PropGet || read.isFreeName())
                continue;
            ValueID holder = index.stripCasts(read.objectOperand());
            if (holders.contains(holder) && findFilledSlot(*this, typeOf(m_frames[visit.frameIndex].references[holder.index()]), read.key()))
                learn(holder, read.key());
        }
    }
    return true;
}

void Walker::noteSlotRead(Reference object, PropertyKeyID key, Reference read)
{
    if (ValueID holder = !m_trialVisits.isEmpty() && object.isValue() && !m_code.isConstant(object.id()) ? underlyingCellOf(m_code, object.id()) : ValueID(); holder && !m_code.isConstant(holder) && m_code.at(holder).opcode == Opcode::Parameter)
        noteSlotOfParameter(holder, key);
    TypeID type = m_trialVisits.isEmpty() || !object.isValue() ? TypeID() : ownTypeOf(read);
    if (!type || !canCarrySlotType(type))
        return;
    ValueID cell = underlyingCellOf(m_code, object.id());
    size_t openLoop = m_openLoops.size();
    for (size_t place = m_trialVisits.size(); place--;) {
        TrialVisit& visit = m_trialVisits[place];
        if (cell.index() >= visit.firstRecord)
            return;
        auto isVisited = [&](const OpenLoop& loop) {
            return loop.frame == visit.frameIndex && m_frames[loop.frame].index->loop(loop.loop).head == visit.inputHead;
        };
        while (openLoop && !isVisited(m_openLoops[openLoop - 1]))
            --openLoop;
        if (!openLoop)
            return;
        if (executesOnEveryIteration(openLoop - 1, currentHoistPoint()))
            visit.slotAccesses.append(SlotAccess { cell, key, underlyingCellOf(m_code, read.id()), { } });
    }
}

static constexpr uint32_t int32PositionWeight = 2;
static constexpr uint32_t methodsWeight = 1 << 16;

void Walker::notePositions(const BodyValue& operation)
{
    const BodyIndex& index = currentBodyIndex();
    Vector<std::pair<ValueID, uint32_t>, 4>& positions = m_trialVisits.last().positions;
    auto note = [&](ValueID operand, uint32_t weight) {
        Reference position = operand ? referenceOf(index.stripCasts(operand)) : Reference();
        TypeID type = position && position.isValue() && !m_code.isConstant(position.id()) ? ownTypeOf(position) : TypeID();
        if (!type || !m_types.at(type).isNumber())
            return;
        ValueID taken = underlyingCellOf(m_code, position.id());
        positions.append({ taken, weight });
        const Operation& made = m_code.at(taken);
        static constexpr OpcodeSet stepsAndSums { Opcode::Increment, Opcode::Decrement, Opcode::Add, Opcode::Sub };
        if (!stepsAndSums.contains(made.opcode))
            return;
        for (ValueID term : made.operands())
            positions.append({ underlyingCellOf(m_code, term), weight });
    };
    if (operation.opcode() != Opcode::Call) {
        Reference object = referenceOf(operation.objectOperand());
        TypeID type = object && object.isRecord() ? typeOf(object) : TypeID();
        return note(operation.propertyOperand(), type && isViewOfNumbersReadInLine(*this, type) ? 1 : int32PositionWeight);
    }
    ValueID callee = index.stripCasts(operation.operand(OperandIndex::Call::callee));
    if (index.isParameter(callee) || index.opcodeOf(callee) != Opcode::PropGet)
        return;
    for (ValueID argument : operation.variadicOperands())
        note(argument, methodsWeight);
}

void Walker::noteSlotStore(Reference object, PropertyKeyID key, Reference before, Reference stored)
{
    TypeID type = m_trialVisits.isEmpty() || !object.isValue() || !stored.isValue() ? TypeID() : ownTypeOf(stored);
    if (!type)
        return;
    ValueID cell = underlyingCellOf(m_code, object.id());
    auto isInt32 = [&](TypeID asked) {
        return asked && m_types.isSubset(asked, TypeTable::int32);
    };
    auto isOwnValue = [&](ValueID operand) {
        return before && before.isValue() && underlyingCellOf(m_code, operand) == underlyingCellOf(m_code, before.id());
    };
    auto isSumWithInt32 = [&] {
        if (!stored.isValue() || m_code.isConstant(stored.id()))
            return false;
        const Operation& sum = m_code.at(underlyingCellOf(m_code, stored.id()));
        if (sum.opcode == Opcode::Increment || sum.opcode == Opcode::Decrement)
            return isOwnValue(sum.operand(0));
        if (sum.opcode != Opcode::Add && sum.opcode != Opcode::Sub)
            return false;
        return (isOwnValue(sum.operand(0)) && isInt32(typeOf(Reference::value(sum.operand(1))))) || (sum.opcode == Opcode::Add && isOwnValue(sum.operand(1)) && isInt32(typeOf(Reference::value(sum.operand(0)))));
    };
    auto isStep = [&](std::initializer_list<Opcode> steps) {
        if (m_code.isConstant(stored.id()))
            return false;
        const Operation& step = m_code.at(underlyingCellOf(m_code, stored.id()));
        return std::ranges::contains(steps, step.opcode) && isOwnValue(step.operand(0));
    };
    std::optional<bool> staysInt32;
    bool stepsByOne = isStep({ Opcode::Increment, Opcode::Decrement });
    bool isStepUp = isStep({ Opcode::Increment });
    for (size_t place = m_trialVisits.size(); place--;) {
        TrialVisit& visit = m_trialVisits[place];
        if (cell.index() >= visit.firstRecord)
            return;
        if (!staysInt32)
            staysInt32 = isInt32(type) || isSumWithInt32();
        visit.slotAccesses.append(SlotAccess { cell, key, before && before.isValue() ? underlyingCellOf(m_code, before.id()) : ValueID(), underlyingCellOf(m_code, stored.id()), *staysInt32, stepsByOne, isStepUp });
    }
}

bool Walker::recordLoopSlots(LoopSummary& summary, TrialVisit& visit)
{
    Vector<SlotAccess, 2>& accesses = visit.slotAccesses;
    if (accesses.isEmpty())
        return false;
    auto keyOf = [](const SlotAccess& access) {
        return std::tuple { access.object, access.key.bits(), !!access.stored };
    };
    std::ranges::sort(accesses, { }, keyOf);
    std::ranges::sort(visit.positions);
    auto weightOfPosition = [&](ValueID value) {
        uint32_t weight = 0;
        for (const auto& [position, weightOfUse] : std::ranges::equal_range(visit.positions, value, { }, &std::pair<ValueID, uint32_t>::first))
            weight += weightOfUse;
        return weight;
    };
    Vector<SlotOfLoop, 2>& slots = summary.storesIntoScopes.slots;
    const BodyIndex& index = *m_frames[visit.frameIndex].index;
    uint32_t firstValueOfLoop = index.block(visit.inputHead).firstParameter().index();
    HashMap<uint32_t, ValueID, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> holders;
    bool hasNamedHolders = false;
    auto typeOfHolder = [&](ValueID holder) {
        return typeOf(m_frames[visit.frameIndex].references[holder.index()]);
    };
    auto isOnlyObject = [&](ValueID holder) {
        TypeID type = typeOfHolder(holder);
        return type && !m_types.at(type).primitives;
    };
    auto nameHolders = [&] {
        if (std::exchange(hasNamedHolders, true))
            return;
        for (BlockID member : index.membersOf(index.loopOf(visit.inputHead))) {
            for (BodyValue operation : index.block(member).values()) {
                operation.forEachOperand([&](ValueID operand, uint32_t) {
                    ValueID holder = operand ? index.stripCasts(operand) : ValueID();
                    Reference named = holder && holder.index() < firstValueOfLoop ? m_frames[visit.frameIndex].references[holder.index()] : Reference();
                    if (!named || !named.isValue())
                        return;
                    auto added = holders.add(underlyingCellOf(m_code, named.id()).index(), holder);
                    if (!added.isNewEntry && !isOnlyObject(added.iterator->value) && isOnlyObject(holder))
                        added.iterator->value = holder;
                });
            }
        }
    };
    bool hasLearnedMore = false;
    for (size_t first = 0; first < accesses.size();) {
        size_t end = first;
        bool isStored = false;
        Vector<ValueID, 8> values;
        bool staysInt32 = true;
        bool isSteppedByOne = true;
        bool onlyGrows = true;
        while (end < accesses.size() && accesses[end].object == accesses[first].object && accesses[end].key == accesses[first].key) {
            isStored |= !!accesses[end].stored;
            for (ValueID value : { accesses[end].before, accesses[end].stored }) {
                if (value)
                    values.append(value);
            }
            staysInt32 &= accesses[end].staysInt32;
            isSteppedByOne &= accesses[end].isStep;
            onlyGrows &= accesses[end].isStepUp;
            ++end;
        }
        SlotAccess access = accesses[first];
        bool isRead = !access.stored;
        first = end;
        std::ranges::sort(values);
        values.shrink(std::ranges::unique(values).begin() - values.begin());
        uint32_t weight = 0;
        for (ValueID value : values)
            weight += weightOfPosition(value);
        bool isInt32Position = staysInt32 && weight >= int32PositionWeight;
        if (!isStored)
            continue;
        nameHolders();
        auto found = holders.find(access.object.index());
        if (found == holders.end())
            continue;
        auto place = std::ranges::lower_bound(slots, std::pair { found->value, access.key.bits() }, { }, &SlotOfLoop::order);
        if (place != slots.end() && place->holder == found->value && place->key == access.key) {
            place->isInt32Position = isInt32Position;
            place->isPosition = weight;
            place->isSteppedByOne = isSteppedByOne && weight < methodsWeight;
            place->onlyGrows = onlyGrows;
            continue;
        }
        if (!isRead || !findFilledSlot(*this, typeOfHolder(found->value), access.key))
            continue;
        slots.insert(place - slots.begin(), SlotOfLoop { found->value, access.key, isInt32Position, !!weight, false, isSteppedByOne && weight < methodsWeight, onlyGrows });
        hasLearnedMore = true;
    }
    return hasLearnedMore;
}

void Walker::findLoopSlots(std::span<const SlotOfLoop> slots, uint32_t firstValueOfLoop, Vector<FieldOfScope, 4>& known, bool shouldReadBeforeHeader)
{
    for (const SlotOfLoop& slot : slots) {
        if (slot.holder.index() >= firstValueOfLoop)
            continue;
        Reference object = referenceOf(slot.holder);
        std::optional<FilledSlot> filled = object && object.isRecord() ? findFilledSlot(*this, typeOf(object), slot.key) : std::nullopt;
        if (!filled)
            continue;
        FieldOfScope found { object, filled->place, TypeID(), Representation::JSValue, { }, { }, { } };
        found.slotKey = slot.key;
        found.receivers = filled->receivers;
        found.slotIsInt32Position = slot.isInt32Position;
        found.slotHolder = slot.holder;
        found.slotIsSteppedByOne = slot.isSteppedByOne;
        found.slotOnlyGrows = slot.onlyGrows;
        if (!slot.hasUnknownSteps)
            found.slotSteps = slot.stepsOfTurn;
        found.mayStartAsInteger = slot.isPosition && !slot.hasLeftIntegers;
        TypeID read = resultTypeOf(draftVariableRead(found));
        if (!read || !canCarrySlotType(read))
            continue;
        Reference value = valueKnownOf(found);
        if (!value && shouldReadBeforeHeader)
            value = build(draftReadBeforeHeader(found, ReadBeforeHeaderUse::LoopCarriedValue));
        if (shouldReadBeforeHeader && isUnreachable())
            return;
        found.values.append(value);
        if (value || !shouldReadBeforeHeader)
            known.append(WTF::move(found));
    }
}

Walker::FieldOfScope Walker::loopCarriedObjectSlot(Reference object, TypeID typeOfObjects, uint32_t position, PropertyKeyID key)
{
    FieldOfScope found;
    std::optional<FilledSlot> filled = object && object.isValue() && typeOfObjects ? findFilledSlot(*this, typeOfObjects, key) : std::nullopt;
    if (!filled)
        return found;
    found.scope = object;
    found.field = filled->place;
    found.slotKey = key;
    found.receivers = filled->receivers;
    found.positionOfHolder = position;
    found.typeOfHolders = typeOfObjects;
    return found;
}

TypeID Walker::typeOfFieldInEveryHolder(const FieldOfScope& field)
{
    Draft read = draftVariableRead(field);
    if (!field.typeOfHolders)
        return resultTypeOf(read);
    typesOfOperands(read);
    m_operandTypes[OperandIndex::GetSlot::object] = field.typeOfHolders;
    return Typed::resultTypeOf(typing(), read, OperandTypes { m_operandTypes.span() });
}

void Walker::findSlotsOfObjectsThatComeRound(BlockID inputHead, const LoopPreview& preview, std::span<const Reference> entering, Vector<FieldOfScope, 4>& known)
{
    if (preview.slots.isEmpty() || preview.effects.maySuspend || m_coldCopyDepth || isUnreachable())
        return;
    BodyBlock head = currentBodyIndex().block(inputHead);
    for (const SlotOfLoop& slot : preview.slots) {
        if (!head.numberOfParameters() || slot.holder < head.firstParameter() || slot.holder.index() - head.firstParameter().index() >= head.numberOfParameters())
            continue;
        uint32_t position = slot.holder.index() - head.firstParameter().index();
        FieldOfScope found = loopCarriedObjectSlot(entering[position], preview.typesOfParameters[position], position, slot.key);
        if (!found.slotKey)
            continue;
        Reference value = valueKnownOf(found);
        if (!value)
            value = build(draftReadBeforeHeader(found, ReadBeforeHeaderUse::LoopCarriedValue));
        if (!value || isUnreachable())
            return;
        found.values.append(value);
        known.append(WTF::move(found));
    }
}

Reference Walker::valueCarriedByLoop(const FieldOfScope& field, std::span<const Reference> arguments) const
{
    return field.positionOfHolder ? m_state.slot(arguments[*field.positionOfHolder], field.field) : valueKnownOf(field);
}

void Walker::findFieldsOfScopesFromOutsideThatQuietTurnsStore(BlockID inputHead, const Effects& ofTurns, std::span<const SlotOfLoop> slots, Vector<FieldOfScope, 4>& known, bool shouldReadBeforeHeader)
{
    bool isTypedBuildWithCalls = !m_versionedLoops.isEmpty() && m_versionedLoops.last().turnsThatRunOtherCodeComeRound && m_versionedLoops.last().frameIndex == m_frames.size() - 1 && m_versionedLoops.last().inputHead == inputHead;
    if (!has(Mechanism::LoopInvariants) || !has(Mechanism::Folding) || m_coldCopyDepth || (ofTurns.mayRunOtherCode() && !isTypedBuildWithCalls) || ofTurns.maySuspend || (shouldReadBeforeHeader && isUnreachable()))
        return;
    const BodyIndex& index = currentBodyIndex();
    findLoopSlots(slots, index.block(inputHead).firstParameter().index(), known, shouldReadBeforeHeader);
    if ((!index.opcodes().contains(Opcode::PutEnvironmentField) && !index.opcodes().contains(Opcode::PutModuleVariable)) || (shouldReadBeforeHeader && isUnreachable()))
        return;
    uint32_t firstValueOfLoop = index.block(inputHead).firstParameter().index();
    LoopID loop = index.loopOf(inputHead);
    BlockID backEdgeDominator;
    for (BlockID member : index.membersOf(loop)) {
        if (!index.isReachable(member))
            continue;
        for (BodyTarget target : index.terminator(member).targets()) {
            if (target.block() != inputHead)
                continue;
            BlockID common = backEdgeDominator ? backEdgeDominator : member;
            while (!index.dominates(common, member))
                common = index.immediateDominator(common);
            backEdgeDominator = common;
        }
    }
    if (!backEdgeDominator)
        return;
    using PlaceRead = std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>;
    Vector<PlaceRead> placesReadInEveryTurn;
    bool placesReadInEveryTurnAreListed = false;
    auto isReadInEveryTurn = [&](ValueID holder, FieldReference place, VariableID variable) {
        if (!std::exchange(placesReadInEveryTurnAreListed, true)) {
            for (BlockID member : index.membersOf(loop)) {
                if (!index.isReachable(member) || !index.dominates(member, backEdgeDominator))
                    continue;
                for (BodyValue operation : index.block(member).values()) {
                    if (operation.opcode() == Opcode::GetModuleVariable)
                        placesReadInEveryTurn.append(PlaceRead { 1, operation.variable().index(), 0, 0 });
                    else if (operation.opcode() == Opcode::GetEnvironmentField)
                        placesReadInEveryTurn.append(PlaceRead { 0, index.stripCasts(operation.environmentOperand()).index(), operation.layout().index(), operation.field() });
                }
            }
            std::ranges::sort(placesReadInEveryTurn);
        }
        return std::ranges::binary_search(placesReadInEveryTurn, variable ? PlaceRead { 1, variable.index(), 0, 0 } : PlaceRead { 0, holder.index(), place.layout.index(), place.field });
    };
    BitVector variablesAccessedEveryIteration;
    Vector<std::pair<uint32_t, ValueID>> anticipatedReads;
    bool hasCollectedAnticipatedReads = false;
    auto findAnticipatedRead = [&](VariableID variable) -> ValueID {
        if (!std::exchange(hasCollectedAnticipatedReads, true)) {
            Vector<BlockID, 8> passed;
            for (BlockID block = backEdgeDominator; passed.isEmpty() || passed.last() != inputHead; block = index.immediateDominator(block))
                passed.append(block);
            for (BlockID block : passed | std::views::reverse) {
                for (BodyValue operation : index.block(block).values()) {
                    if ((operation.opcode() == Opcode::GetModuleVariable || operation.opcode() == Opcode::PutModuleVariable) && variablesAccessedEveryIteration.add(operation.variable().index()) && operation.opcode() == Opcode::GetModuleVariable)
                        anticipatedReads.append({ operation.variable().index(), operation.id() });
                }
            }
            std::ranges::sort(anticipatedReads, { }, &std::pair<uint32_t, ValueID>::first);
        }
        auto found = std::ranges::lower_bound(anticipatedReads, variable.index(), { }, &std::pair<uint32_t, ValueID>::first);
        return found != anticipatedReads.end() && found->first == variable.index() ? found->second : ValueID();
    };
    for (BlockID member : index.membersOf(loop)) {
        for (BodyValue operation : index.block(member).values()) {
            VariableID variable = operation.opcode() == Opcode::PutModuleVariable ? operation.variable() : VariableID();
            if (operation.opcode() != Opcode::PutEnvironmentField && !variable)
                continue;
            if (variable && (isVariableFilledAtFirstRead(plan(), variable) || plan().promotedEnvironments().find(variable) || plan().deferredDefinitions().findDeferred(variable)))
                continue;
            if (variable && ofTurns.mayRunOtherCode() && program().at(variable).initialization == VariableInitialization::Uninitialized && !leavesDeadZoneForGood(program(), variable))
                continue;
            ValueID holder = variable ? ValueID() : index.stripCasts(operation.environmentOperand());
            Reference scope = holder && holder.index() < firstValueOfLoop ? referenceOf(holder) : Reference();
            FieldReference place = variable ? FieldReference { } : FieldReference { operation.layout(), operation.field() };
            if (!variable && (!scope || !scope.isRecord() || isEnvironmentAllocation(scope)))
                continue;
            auto readsPlace = [&](ValueID written) {
                ValueID read = index.stripCasts(written);
                if (!index.isParameter(read) && index.opcodeOf(read) == Opcode::CheckInitialized)
                    read = index.stripCasts(index.value(read).operand(0));
                if (variable)
                    return !index.isParameter(read) && index.opcodeOf(read) == Opcode::GetModuleVariable && index.value(read).variable() == variable;
                if (index.isParameter(read) || index.opcodeOf(read) != Opcode::GetEnvironmentField)
                    return false;
                BodyValue found = index.value(read);
                return index.stripCasts(found.environmentOperand()) == holder && found.layout() == place.layout && found.field() == place.field;
            };
            auto readOfPlaceTakenAlone = [&](ValueID written) -> ValueID {
                ValueID read = written;
                for (;;) {
                    if (index.isParameter(read) || !index.hasOneUse(read))
                        return { };
                    if (variable && index.opcodeOf(read) == Opcode::GetModuleVariable && index.value(read).variable() == variable)
                        return read;
                    if (!variable && index.opcodeOf(read) == Opcode::GetEnvironmentField && index.stripCasts(index.value(read).environmentOperand()) == holder && index.value(read).layout() == place.layout && index.value(read).field() == place.field)
                        return read;
                    ValueID below = index.stripCasts(read);
                    if (below == read) {
                        if (index.opcodeOf(read) != Opcode::CheckInitialized)
                            return { };
                        below = index.value(read).operand(0);
                    }
                    read = below;
                }
            };
            ValueID stored = index.stripCasts(operation.valueOperand());
            static constexpr OpcodeSet stepsOfOne { Opcode::Increment, Opcode::Decrement };
            bool isStep = !index.isParameter(stored) && stepsOfOne.contains(index.opcodeOf(stored)) && readsPlace(index.value(stored).operand(0));
            static constexpr OpcodeSet operatorsOfSums { Opcode::Add, Opcode::Sub, Opcode::Mul };
            bool isComputedFromItself = isStep || (!index.isParameter(stored) && operatorsOfSums.contains(index.opcodeOf(stored)) && (readsPlace(index.value(stored).operand(0)) || readsPlace(index.value(stored).operand(1))));
            static constexpr OpcodeSet operatorsThatAdd { Opcode::Add, Opcode::Sub };
            ValueID added;
            if (!isStep && isComputedFromItself && operatorsThatAdd.contains(index.opcodeOf(stored)) && (index.opcodeOf(stored) == Opcode::Add || readsPlace(index.value(stored).operand(0))))
                added = readsPlace(index.value(stored).operand(0)) ? index.value(stored).operand(1) : index.value(stored).operand(0);
            bool isStepOrAddition = isStep || added;
            ValueID readTakenBySum;
            if (isStepOrAddition) {
                readTakenBySum = readOfPlaceTakenAlone(index.value(stored).operand(0));
                if (!readTakenBySum && !isStep)
                    readTakenBySum = readOfPlaceTakenAlone(index.value(stored).operand(1));
            }
            size_t listed = known.findIf([&](const FieldOfScope& field) {
                return field.variable == variable && !field.slotKey && field.scope == scope && field.field.layout == place.layout && field.field.field == place.field;
            });
            if (listed != notFound) {
                known[listed].isSteppedByOne &= isStep;
                known[listed].isComputedFromItself &= isComputedFromItself;
                known[listed].isSteppedOrAddedTo &= isStepOrAddition;
                if (added)
                    known[listed].addedInTurns.append(added);
                known[listed].storedInTurns.append(stored);
                if (readTakenBySum)
                    known[listed].readsTakenBySums.append(readTakenBySum);
                continue;
            }
            if (!isReadInEveryTurn(holder, place, variable))
                continue;
            FieldOfScope found { scope, place, TypeID(), Representation::JSValue, { }, { }, { }, false, isStep, isComputedFromItself, { }, variable };
            found.isSteppedOrAddedTo = isStepOrAddition;
            if (added)
                found.addedInTurns.append(added);
            found.storedInTurns.append(stored);
            if (readTakenBySum)
                found.readsTakenBySums.append(readTakenBySum);
            if (variable)
                found.anticipatedRead = findAnticipatedRead(variable);
            if (Draft read = draftVariableRead(found); variable && !found.anticipatedRead && !m_state.isPastDeadZone(variable) && excludesInitialPrimitive(read, resultTypeOf(read)) && !shouldAssumeVariablesInitialized())
                continue;
            Reference value = valueKnownOf(found);
            if (!value && shouldReadBeforeHeader && !mayBeInDeadZone(found)) {
                if (variable)
                    count(StatisticID::LoopStoredVariableReadsHoisted);
                value = build(draftReadBeforeHeader(found, ReadBeforeHeaderUse::LoopCarriedValue));
            }
            found.values.append(value);
            if (value || !shouldReadBeforeHeader)
                known.append(WTF::move(found));
        }
    }
    auto keyOfPlace = [](VariableID variable, FieldReference place) {
        return variable ? uint64_t { 1 } << 63 | variable.index() : static_cast<uint64_t>(place.layout.index()) << 32 | place.field;
    };
    HashMap<uint64_t, int32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> readsOfAccumulators;
    for (FieldOfScope& field : known) {
        if (!field.isSteppedOrAddedTo || field.readsTakenBySums.size() != field.storedInTurns.size())
            continue;
        readsOfAccumulators.add(keyOfPlace(field.variable, field.field), 0);
    }
    if (readsOfAccumulators.isEmpty())
        return;
    auto readsCountedOf = [&](BodyValue operation) -> int32_t* {
        if (operation.opcode() != Opcode::GetModuleVariable && operation.opcode() != Opcode::GetEnvironmentField)
            return nullptr;
        bool readsVariable = operation.opcode() == Opcode::GetModuleVariable;
        auto found = readsOfAccumulators.find(keyOfPlace(readsVariable ? operation.variable() : VariableID(), readsVariable ? FieldReference { } : FieldReference { operation.layout(), operation.field() }));
        return found == readsOfAccumulators.end() ? nullptr : &found->value;
    };
    for (BlockID member : index.membersOf(loop)) {
        for (BodyValue operation : index.block(member).values()) {
            if (int32_t* reads = readsCountedOf(operation))
                ++*reads;
            if (operation.opcode() != Opcode::CheckInitialized || !index.isUnused(operation.id()))
                continue;
            ValueID checked = index.stripCasts(operation.operand(0));
            if (index.isParameter(checked) || !index.hasOneUse(checked))
                continue;
            if (int32_t* reads = readsCountedOf(index.value(checked)))
                --*reads;
        }
    }
    for (FieldOfScope& field : known) {
        if (!field.isSteppedOrAddedTo || field.readsTakenBySums.size() != field.storedInTurns.size())
            continue;
        field.isReadBySumsAlone = readsOfAccumulators.get(keyOfPlace(field.variable, field.field)) == static_cast<int32_t>(field.readsTakenBySums.size());
    }
}

TypeID Walker::backEdgeTypeOfVariable(BlockID inputHead, const FieldOfScope& field) const
{
    if (!inputHead || m_versionedLoops.isEmpty())
        return { };
    const VersionedLoop& versionedLoop = m_versionedLoops.last();
    if (versionedLoop.frameIndex != m_frames.size() - 1 || versionedLoop.inputHead != inputHead || !versionedLoop.entry)
        return { };
    for (const FieldOfScope& held : versionedLoop.variableBackEdgeChecks) {
        if (held.variable == field.variable && held.slotKey == field.slotKey && held.scope == field.scope && held.field.layout == field.field.layout && held.field.field == field.field.field)
            return held.type;
    }
    return { };
}

bool Walker::mayBeInDeadZone(const FieldOfScope& field) const
{
    if (field.variable)
        return program().at(field.variable).initialization == VariableInitialization::Uninitialized && !m_state.isPastDeadZone(field.variable);
    if (field.slotKey)
        return false;
    TypeID written = program().at(field.field.layout).fields[field.field.field].declared.written();
    return written && (program().at(written).primitives & Primitive::Absent) && !m_state.isPastDeadZone(field.scope, field.field);
}

Draft Walker::draftVariableRead(const FieldOfScope& field)
{
    if (field.slotKey) {
        Draft read(Opcode::GetSlot, m_origin);
        chooseConservativeFields(read);
        read.setObjectOperand(field.scope);
        read.setKey(field.slotKey);
        read.setReceivers(field.receivers);
        read.setLevel(KnowledgeLevel::Proven);
        return read;
    }
    if (!field.variable)
        return draftCapturedVariableRead(field.scope, field.field);
    Draft read(Opcode::GetModuleVariable, m_origin);
    read.setVariable(field.variable);
    chooseConservativeFields(read);
    read.setIsKnownInitialized(true);
    return read;
}

Draft Walker::draftReadBeforeHeader(const FieldOfScope& field, ReadBeforeHeaderUse use, bool moduleMayBeUnevaluated)
{
    Draft read = draftVariableRead(field);
    if (field.slotKey)
        return read;
    if (!field.variable) {
        if (use == ReadBeforeHeaderUse::GuardOperand && !shouldAssumeSlotsInitialized())
            read.setIsKnownInitialized(false);
        return read;
    }
    if (field.anticipatedRead) {
        read.setOrigin(originOf(currentBodyIndex().value(field.anticipatedRead)));
        read.inputValueGiven = field.anticipatedRead;
        return read;
    }
    if (shouldAssumeVariablesInitialized())
        return read;
    read.setIsKnownInitialized(false);
    if (moduleMayBeUnevaluated || (!m_state.isPastDeadZone(field.variable) && excludesInitialPrimitive(read, resultTypeOf(read)))) {
        read.setReturnsRawSlot(true);
        count(StatisticID::RawVariableReadsHoisted);
    } else if (m_state.isPastDeadZone(field.variable))
        read.setIsKnownInitialized(true);
    return read;
}

Draft Walker::draftCapturedVariableRead(Reference scope, FieldReference field)
{
    Draft read(Opcode::GetEnvironmentField, m_origin);
    read.setEnvironmentOperand(scope);
    read.setLayout(field.layout);
    read.setField(field.field);
    chooseConservativeFields(read);
    read.setIsKnownInitialized(true);
    return read;
}

bool Walker::carriedFieldsPartWays(std::span<const uint32_t> passing, std::span<const uint32_t> closed)
{
    for (const OpenLoop& loop : m_openLoops) {
        auto fields = m_fieldsOfScopesOfHeads.find(loop.head.index());
        if (fields == m_fieldsOfScopesOfHeads.end())
            continue;
        for (const FieldOfScope& field : fields->value) {
            auto fitsHead = [&](uint32_t place) {
                const WalkState& state = m_openEdges[place].state;
                Reference value = field.variable ? state.variable(field.variable) : field.slotKey ? state.slot(field.scope, field.field) : state.environmentField(field.scope, field.field);
                TypeID type = value ? ownTypeOf(value) : TypeID();
                return type && m_types.isSubset(type, field.type);
            };
            if (std::ranges::all_of(passing, fitsHead) && !std::ranges::all_of(closed, fitsHead))
                return true;
        }
    }
    return false;
}

void Walker::noteFieldOfScope(const FieldOfScope& field, Reference value)
{
    if (field.carriedResult)
        noteKnown(*this, KnownNote::CarriedResult, field.carriedResult, { }, 0, { }, value.bits());
    else if (field.variable)
        m_state.noteVariable(field.variable, value);
    else if (field.slotKey) {
        m_state.noteSlot(field.scope, field.field, value);
        if (isInLoop())
            noteStoreIntoUnmadeRecord();
    } else
        noteEnvironmentField(field.scope, field.field, value);
}

void Walker::renameCarriedFields(Reference value, Reference name)
{
    for (const OpenLoop& loop : m_openLoops) {
        auto fields = m_fieldsOfScopesOfHeads.find(loop.head.index());
        if (fields == m_fieldsOfScopesOfHeads.end())
            continue;
        for (const FieldOfScope& field : fields->value) {
            if (valueKnownOf(field) == value)
                noteFieldOfScope(field, name);
        }
    }
}

bool Walker::narrowCarriedFields(EdgeID& edge, BlockID from, BlockID to, uint32_t targetIndex)
{
    const Terminator& end = m_code.at(from).terminator;
    if (!reckonsOnWhatItHasBuilt() || !end.isConditionalBranch() || m_code.isConstant(end.conditionOperand()))
        return true;
    bool isOnTakenEdge = !m_edgeOfCodeBeingLed.value_or(targetIndex);
    ValueID relation = end.conditionOperand();
    for (Opcode opcode = m_code.at(relation).opcode; opcode == Opcode::LogicalNot || opcode == Opcode::ToBoolean || conversionsOfForm.contains(opcode); opcode = m_code.at(relation).opcode) {
        isOnTakenEdge ^= opcode == Opcode::LogicalNot;
        relation = m_code.at(relation).operand(0);
        if (m_code.isConstant(relation))
            return true;
    }
    const Operation& test = m_code.at(relation);
    const Operation* read = test.opcode == Opcode::TypeTest && !m_code.isConstant(test.valueOperand()) ? &m_code.at(test.valueOperand()) : nullptr;
    if (read && read->opcode == Opcode::GetArrayElement) {
        Reference element = Reference::value(test.valueOperand());
        Reference array = Reference::value(read->arrayOperand());
        Reference position = Reference::value(read->positionOperand());
        Draft narrowed(Opcode::Narrowed, m_origin);
        narrowed.setValueOperand(element);
        narrowed.setNarrowsHolderOfOperand(false);
        narrowed.setNarrowsTermOfOperand(false);
        narrowed.setTestOperand(Reference::value(relation));
        narrowed.setOperandIndex(OperandIndex::TypeTest::value);
        narrowed.setIsOnTakenEdge(isOnTakenEdge);
        TypeID type = findElementRead(*this, array, position) == element ? resultTypeOf(narrowed) : TypeID();
        if (!type || type == read->type)
            return true;
        if (!m_block) {
            m_block = addBlockFor(to);
            m_code.closeEdge(edge, m_block, { });
            edge = { };
        }
        Reference name = build(WTF::move(narrowed));
        if (isUnreachable())
            return false;
        renameElementRead(*this, array, position, name);
        return true;
    }
    if (provesOf(test.opcode) != Proof::IntegerBound)
        return true;
    Vector<std::pair<Reference, uint32_t>, 2> asked;
    for (uint32_t operandIndex = 0; operandIndex < 2; ++operandIndex) {
        ValueID operand = underlyingCellOf(m_code, m_code.at(relation).operand(operandIndex));
        for (const OpenLoop& loop : m_openLoops) {
            auto fields = m_fieldsOfScopesOfHeads.find(loop.head.index());
            if (!operand || fields == m_fieldsOfScopesOfHeads.end())
                continue;
            for (const FieldOfScope& field : fields->value) {
                Reference value = valueKnownOf(field);
                std::pair found { value, operandIndex };
                if (value && value.isValue() && !m_code.isConstant(value.id()) && underlyingCellOf(m_code, value.id()) == operand && !asked.contains(found))
                    asked.append(found);
            }
        }
    }
    for (auto [value, operandIndex] : asked) {
        Draft narrowed(Opcode::Narrowed, m_origin);
        narrowed.setValueOperand(value);
        narrowed.setNarrowsHolderOfOperand(false);
        narrowed.setNarrowsTermOfOperand(false);
        narrowed.setTestOperand(Reference::value(relation));
        narrowed.setOperandIndex(operandIndex);
        narrowed.setIsOnTakenEdge(isOnTakenEdge);
        TypeID type = resultTypeOf(narrowed);
        if (!type || type == m_code.at(value.id()).type)
            continue;
        if (!m_block) {
            m_block = addBlockFor(to);
            m_code.closeEdge(edge, m_block, { });
            edge = { };
        }
        Reference name = build(WTF::move(narrowed));
        if (isUnreachable())
            return false;
        renameCarriedFields(value, name);
        count(StatisticID::CarriedFieldsNarrowedByCopies);
    }
    return true;
}

bool Walker::isFitForParameterOfField(const FieldOfScope& field, Reference value)
{
    TypeID type = ownTypeOf(value);
    return type && m_types.isSubset(type, field.type) && representationOf(value) != Representation::Void && !isClosureLeftUnmade(*this, value) && (!field.shown || isShown(value, field.shown));
}

Reference Walker::fitForParameterOfField(const FieldOfScope& field, Reference known)
{
    return isFitForParameterOfField(field, known) ? known : Reference();
}

bool Walker::mayHandFieldOfScopeToBlock(FieldOfScope& field, BlockID inputHeadOfLoopEntered)
{
    Reference entering = field.values[0];
    if (!field.variable && !field.slotKey) {
        const Vector<Field>& fields = program().at(field.field.layout).fields;
        if (field.field.field >= fields.size() || fields[field.field.field].declared.isEstablished())
            return false;
    }
    if (namesFreshCell(m_state, entering))
        return false;
    field.type = typeOfFieldInEveryHolder(field);
    if (!field.type || !m_types.at(field.type).isJavaScriptValue())
        return false;
    if (TypeID held = backEdgeTypeOfVariable(inputHeadOfLoopEntered, field), own = ownTypeOf(entering); held && own && m_types.isSubset(own, held) && !m_types.isSubset(field.type, held)) {
        field.type = held;
        field.isHeld = true;
    } else if (TypeID integer = m_types.integer(field.startingIntegers.isEmpty() ? Range::exact() : field.startingIntegers); field.mayStartAsInteger && inputHeadOfLoopEntered && own && hasOpenTrialVisit(m_frames.size() - 1, inputHeadOfLoopEntered)
        && m_types.isSubset(own, integer) && m_types.isSubset(integer, field.type) && !m_types.isSubset(field.type, integer)) {
        field.type = integer;
        field.startsAsInteger = true;
        if (!field.slotKey)
            count(StatisticID::CapturedCountersCarriedAsIntegers);
    }
    field.form = formOfParameter(field.type, { });
    if (!field.slotKey && (field.form == Representation::JSValue || optionalRepresentations.contains(field.form)) && isEnforced(ProofSource::SlotOfScope))
        field.shown = typeThatUnitMayTestFor(m_types.testablePartOf(field.type), true, true);
    return isFitForParameterOfField(field, entering);
}

void Walker::noteWhatParameterOfFieldIsShownToBe(const FieldOfScope& field)
{
    if (field.shown)
        setTypeAsked(field.parameter.id(), field.shown);
}

void Walker::keepFieldsOfScopesThatLoopWrites(BlockID inputHead, Vector<FieldOfScope, 4>& known)
{
    known.removeAllMatching([&](FieldOfScope& field) {
        if ((!field.positionOfHolder && valueKnownOf(field)) || !mayHandFieldOfScopeToBlock(field, inputHead))
            return true;
        field.values[0] = fitForParameterOfField(field, field.values[0]);
        return !field.values[0];
    });
    for (const FieldOfScope& field : known) {
        if (!field.variable && !field.slotKey && isEnvironmentAllocation(field.scope))
            m_scopesWhoseFieldsHaveComeRound.add(field.scope.id().index());
    }
}

bool Walker::bringFieldsOfScopesRound(EdgeID& edge, BlockID head, Vector<Reference, 8>& arguments)
{
    auto fields = m_fieldsOfScopesOfHeads.find(head.index());
    if (fields == m_fieldsOfScopesOfHeads.end())
        return true;
    auto comesRoundAsKnown = [&](const FieldOfScope& field) {
        return isFitForParameterOfField(field, valueCarriedByLoop(field, arguments.span()));
    };
    if (edge && !std::ranges::all_of(fields->value, comesRoundAsKnown))
        layBlockOnEdge(edge, head);
    size_t numberBesideFieldsOfScopes = arguments.size();
    for (const FieldOfScope& field : fields->value) {
        Reference value = fitForParameterOfField(field, valueCarriedByLoop(field, arguments.span()));
        if (TrialVisit* visit = !value && field.startsAsInteger ? findTryOfBlockAheadOfEdges(head) : nullptr) {
            for (SlotOfLoop& slot : m_loopSummaries[visit->summaryIndex].storesIntoScopes.slots)
                slot.hasLeftIntegers |= slot.holder == field.slotHolder && slot.key == field.slotKey;
            for (CapturedCounter& counter : m_loopSummaries[visit->summaryIndex].storesIntoScopes.counters)
                counter.hasLeftIntegers |= !field.slotKey && counter.field == field.field;
            visit->misfits = true;
            value = field.parameter;
        }
        if (!value && field.isHeld && !m_versionedLoops.isEmpty() && backEdgeTypeOfVariable(m_versionedLoops.last().inputHead, field) == field.type) {
            value = askAgainWhereItComesRound(edge, head, field.type, arguments.span().first(numberBesideFieldsOfScopes), scopedLambda<Reference()>([&] {
                if (Reference atHand = valueKnownOf(field))
                    return atHand;
                return build(draftReadBeforeHeader(field, ReadBeforeHeaderUse::LoopCarriedValue));
            }));
        }
        if (!value && field.positionOfHolder && findTryOfBlockAheadOfEdges(head)) {
            TypeID comingRound = ownTypeOf(arguments[*field.positionOfHolder]);
            if (!comingRound || !m_types.isSubset(comingRound, m_code.at(m_code.at(head).parameters()[*field.positionOfHolder]).type))
                value = field.parameter;
        }
        if (!value && !field.isHeld) {
            Draft read = draftReadBeforeHeader(field, ReadBeforeHeaderUse::LoopCarriedValue);
            if (field.positionOfHolder)
                read.setObjectOperand(arguments[*field.positionOfHolder]);
            value = build(WTF::move(read));
        }
        if (!value) {
            if (!m_hasFailed && !isUnreachable())
                fail("a field of a scope does not come round to the head of its loop"_s);
            m_block = { };
            return false;
        }
        if (TrialVisit* visit = !field.slotKey && !field.variable && !field.startsAsInteger && isEnvironmentAllocation(field.scope) ? findTryOfBlockAheadOfEdges(head) : nullptr)
            noteCapturedCounter(*visit, field, value);
        if (TrialVisit* visit = field.slotKey ? findTryOfBlockAheadOfEdges(head) : nullptr) {
            for (SlotOfLoop& slot : m_loopSummaries[visit->summaryIndex].storesIntoScopes.slots) {
                if (slot.holder != field.slotHolder || slot.key != field.slotKey || slot.hasUnknownSteps)
                    continue;
                std::optional<Range> steps = value.isValue() && field.parameter.isValue() ? findStepsFrom(m_code, m_types, value.id(), field.parameter.id(), StepsAreExact::AsAssumed) : std::nullopt;
                slot.hasUnknownSteps = !steps;
                slot.stepsOfTurn = steps ? slot.stepsOfTurn.unionWith(*steps) : slot.stepsOfTurn;
            }
        }
        arguments.append(value);
    }
    return true;
}

void Walker::addScopeFieldParameters(BlockID entry, const LoopPreview& look)
{
    if (look.scopesThatTurnsOnlyBorrow.isEmpty())
        return;
    Vector<FieldOfScope, 4> fields;
    findFieldsOfScopesInFrontOfLoop(look, fields);
    fields.removeAllMatching([&](FieldOfScope& field) {
        return !mayHandFieldOfScopeToBlock(field);
    });
    if (fields.isEmpty())
        return;
    for (FieldOfScope& field : fields) {
        field.parameter = addParameter(entry, field.type, field.form);
        noteWhatParameterOfFieldIsShownToBe(field);
    }
    fieldsOfScopesLaidAt(entry).append(fields.span());
}

void Walker::addParametersForSlotsOfObjects(BlockID entry, BlockID inputHead, const LoopPreview& look, std::span<const Reference> parameters)
{
    if (look.slots.isEmpty() || look.effects.maySuspend || m_coldCopyDepth)
        return;
    BodyBlock head = currentBodyIndex().block(inputHead);
    for (const SlotOfLoop& slot : look.slots) {
        if (!head.numberOfParameters() || slot.holder < head.firstParameter() || slot.holder.index() - head.firstParameter().index() >= head.numberOfParameters())
            continue;
        uint32_t position = slot.holder.index() - head.firstParameter().index();
        FieldOfScope found = loopCarriedObjectSlot(parameters[position], typeOf(parameters[position]), position, slot.key);
        found.type = found.slotKey ? resultTypeOf(draftVariableRead(found)) : TypeID();
        if (!found.type || !m_types.at(found.type).isJavaScriptValue())
            continue;
        found.form = formOfParameter(found.type, { });
        found.parameter = addParameter(entry, found.type, found.form);
        fieldsOfScopesLaidAt(entry).append(WTF::move(found));
    }
}

Vector<Walker::FieldOfScope>& Walker::fieldsOfScopesLaidAt(BlockID block)
{
    auto laid = m_fieldsOfScopesOfHeads.add(block.index(), Vector<FieldOfScope> { });
    if (laid.isNewEntry)
        m_blocksWithFieldsOfScopesInOrder.append(block.index());
    return laid.iterator->value;
}

void Walker::noteFieldsOfScopesThatBlockTakes(BlockID block)
{
    if (auto fields = m_fieldsOfScopesOfHeads.find(block.index()); fields != m_fieldsOfScopesOfHeads.end())
        noteFieldsOfScopes(fields->value.span());
}

void Walker::forgetAllButFreshObjectsAndFieldsThatBlockTakes(BlockID block)
{
    Vector<Reference, 4> known;
    auto fields = m_fieldsOfScopesOfHeads.find(block.index());
    if (fields != m_fieldsOfScopesOfHeads.end()) {
        for (const FieldOfScope& field : fields->value)
            known.append(m_state.environmentField(field.scope, field.field));
    }
    m_state.forgetAllButFreshObjects();
    for (size_t index = 0; index < known.size(); ++index) {
        if (known[index])
            noteEnvironmentField(fields->value[index].scope, fields->value[index].field, known[index]);
    }
}

size_t Walker::numberOfFieldsOfScopesThatBlockTakes(BlockID block) const
{
    auto fields = m_fieldsOfScopesOfHeads.find(block.index());
    const OpenLoop* loop = m_carriedResultRequests.isEmpty() ? nullptr : openLoopOfHeader(block);
    return (fields == m_fieldsOfScopesOfHeads.end() ? 0 : fields->value.size()) + (loop ? loop->carriedResultCount : 0);
}

bool Walker::unitKeepsScopesToItself() const
{
    return has(Mechanism::EscapeAnalysis) && unitMayLendWhatItMakes(program(), unit());
}

auto Walker::accessesOfScopeByFunction(FunctionID function, uint32_t entryParameter) -> const AccessesOfScopeByFunction&
{
    return m_accessesOfScopesByFunctions.ensure(static_cast<uint64_t>(function.index()) << 32 | entryParameter, [&] {
        AccessesOfScopeByFunction found;
        const Function& entered = program().at(function);
        const BodyIndex* body = facts().bodies().find(function);
        if (!body || !body->numberOfBlocks() || entered.isCoroutine() || entered.calleeParameterIndex() || entered.has(FunctionCapabilities::RecordsCallee) || facts().summaries().at(function).has(SummaryTrait::HasDirectEval))
            return found;
        BodyBlock entry = body->block(entryBlock);
        if (entryParameter >= entered.captures.size() || entryParameter >= entry.numberOfParameters())
            return found;
        ValueID scope = entry.parameter(entryParameter);
        uint32_t mentions = body->useCount(scope);
        if (mentions == BodyIndex::maxCountedUses)
            return found;
        uint32_t accesses = 0;
        for (uint32_t number = 0; number < body->numberOfBlocks() && accesses < mentions; ++number) {
            for (BodyValue operation : body->block(BlockID(number)).values()) {
                Opcode opcode = operation.opcode();
                if ((opcode != Opcode::GetEnvironmentField && opcode != Opcode::PutEnvironmentField) || operation.environmentOperand() != scope)
                    continue;
                ++accesses;
                found.fieldsAccessed.appendIfNotContains(FieldReference { operation.layout(), operation.field() });
                if (opcode == Opcode::PutEnvironmentField)
                    found.fieldsStored.appendIfNotContains(FieldReference { operation.layout(), operation.field() });
            }
        }
        found.areAllItsMentions = accesses == mentions;
        return found;
    }).iterator->value;
}

bool Walker::operationOnlyBorrowsScope(const Draft& draft, Reference scope, uint32_t operandIndex, bool isHeldByClosure)
{
    if (!isEnvironmentAllocation(scope) || !unitKeepsScopesToItself())
        return false;
    if (draft.opcode == Opcode::Call)
        return isHeldByClosure && operandIndex == OperandIndex::Call::callee;
    if (draft.opcode == Opcode::CallResolved)
        return isHeldByClosure && operandIndex == OperandIndex::CallResolved::callee;
    uint32_t numberOfNamedOperands = draft.numberOfNamedOperands();
    if (operandIndex < numberOfNamedOperands)
        return false;
    uint32_t position = operandIndex - numberOfNamedOperands;
    if (draft.opcode == Opcode::CallDirect) {
        if (isHeldByClosure)
            return functionEnteredOnlyCallsWhatArrivesAt(facts(), draft.role(), draft.numberOfBoundArguments(), draft.function(), position);
        return returnsResultOfFunction(draft.role()) && !draft.numberOfBoundArguments() && functionOnlyReadsAndWritesScopeAt(draft.function(), position);
    }
    if (draft.opcode != Opcode::NewClosure || isHeldByClosure || draft.isHeldByClosureTable() || draft.isMadeOnDemand())
        return false;
    if (!closureHasCellThatMayBeLent(program(), draft.closureForm(), draft.function()) || facts().summaries().at(currentFunction()).has(SummaryTrait::HasDirectEval))
        return false;
    if (!functionOnlyReadsAndWritesScopeAt(draft.function(), position))
        return false;
    m_scopesThatClosuresHold.add(scope.id().index());
    return true;
}

void Walker::noteThatWalkLetsScopesNamedLeave(Reference reference)
{
    bool nothingIsKeptAtHeads = std::ranges::all_of(m_trialVisits, [](const TrialVisit& trialVisit) {
        return trialVisit.freshObjectsKept.isEmpty();
    });
    if (nothingIsKeptAtHeads)
        return;
    Vector<Reference, 2> scopes;
    listScopesThatValueNames(reference, scopes);
    for (Reference scope : scopes)
        noteThatWalkLetsScopeLeave(scope);
}

template<typename Functor>
bool Walker::everyOpenLoopAround(const Functor& holdsOfLoopAt) const
{
    for (const Frame& frame : m_frames) {
        if (frame.placesOfOpenLoops.isEmpty())
            continue;
        for (LoopID loop = frame.index->loopOf(frame.block); loop; loop = frame.index->loop(loop).parent) {
            uint32_t place = frame.placesOfOpenLoops[loop.index()];
            if (place && !holdsOfLoopAt(place - 1))
                return false;
        }
    }
    return true;
}

void Walker::noteThatWalkLetsScopeLeave(Reference scope)
{
    if (m_trialVisits.isEmpty() || !isEnvironmentAllocation(scope) || !m_state.isFresh(scope))
        return;
    for (TrialVisit& trialVisit : m_trialVisits) {
        for (const FreshObjectKeptAtHead& kept : trialVisit.freshObjectsKept) {
            if (kept.object == scope)
                loseObjectKeptAtHead(trialVisit, kept);
        }
    }
}

void Walker::endFieldsOfScopesThatTurnsStore(const LoopPreview& look)
{
    Vector<Reference, 2> scopes;
    for (Reference scope : look.scopesThatTurnsOnlyBorrow) {
        if (m_state.isFresh(scope))
            scopes.append(scope);
    }
    if (scopes.isEmpty() || look.fieldsOfScopesThatTurnsStore.isEmpty())
        return;
    Effects stores = Effects::none();
    for (FieldReference field : look.fieldsOfScopesThatTurnsStore)
        stores.writes.add(program(), MemoryLocation::environmentField(field.layout, field.field));
    for (Reference scope : scopes) {
        for (FieldReference field : std::ranges::equal_range(look.fieldsOfScopesThatTurnsStore, m_code.at(scope.id()).layout(), { }, &FieldReference::layout))
            m_state.noteEnvironmentField(scope, field, { });
        m_state.answers().endWhatIsWrittenIn(scope, stores);
    }
    m_state.invalidate(stores);
}

void Walker::enterGenericVersion(BlockID entry, const LoopPreview& look)
{
    if (!has(Mechanism::EscapeAnalysis) || !look.hasLetMentionsLeave || look.effects.maySuspend || !m_state.holdsFreshObjects())
        m_state.clear();
    else {
        endFieldsOfScopesThatTurnsStore(look);
        m_state.invalidate(look.effects);
        m_state.forgetAllButFreshObjects();
        Vector<Reference, 2> scopes;
        for (Reference scope : look.scopesThatTurnsOnlyBorrow) {
            if (m_state.isFresh(scope))
                scopes.append(scope);
        }
        for (Reference scope : scopes)
            m_state.markEscaped(scope);
        if (!scopes.isEmpty())
            m_state.invalidateHeap();
        for (Reference scope : scopes)
            m_state.noteFresh(scope);
    }
    noteFieldsOfScopesThatBlockTakes(entry);
    standAheadOfEdges(entry);
}

Reference Walker::buildCopyOfScope(const ScopeLentToCall& loan, Origin origin)
{
    LayoutID layout = m_code.at(loan.scope.id()).layout();
    const PlannedLayout& planned = plan().layout(layout);
    if (planned.environmentOfDeferredClosures || m_code.at(loan.scope.id()).prototypeOperand())
        return { };
    Vector<FieldIndex, 8> fields;
    Vector<Reference, 8> values;
    for (FieldIndex field : loan.fieldsAccessed) {
        const PlacedSlot& slot = planned.slots[field];
        if (!slot.hasStorage())
            continue;
        Reference value = m_state.environmentField(loan.scope, { layout, field });
        TypeID type = value ? typeOf(value) : TypeID();
        if (!type || (m_types.at(type).primitives & Primitive::Absent) || representationOf(value) == Representation::Void || slot.representation == SlotRepresentation::DeferredClosure)
            return { };
        if (isClosureLeftUnmade(*this, value) || namesFreshCell(m_state, value) || (slot.type && !m_types.isSubset(type, slot.type)))
            return { };
        fields.append(field);
        values.append(value);
    }
    Draft copy(Opcode::NewObject, origin);
    copy.standsForNoValueOfInput = true;
    copy.setLayout(layout);
    copy.setFields(fields.span());
    copy.setValuesOperands(values.span());
    chooseConservativeFields(copy);
    return isUnreachable() ? Reference() : build(WTF::move(copy));
}

void Walker::lendCopiesOfScopesTo(Draft& call, Vector<ScopeLentToCall, 1>& lent)
{
    if (isUnreachable() || call.opcode != Opcode::CallDirect || !m_state.holdsFreshObjects() || !unitKeepsScopesToItself() || isUnderHandler())
        return;
    bool standsOnRareWay = m_code.at(m_block).frequency == BlockFrequency::Rare && everyOpenLoopAround([&](uint32_t place) {
        return m_code.at(m_openLoops[place].head).frequency != BlockFrequency::Rare;
    });
    if (!standsOnRareWay && m_scopesWhoseFieldsHaveComeRound.isEmpty())
        return;
    auto cellNamedBy = [&](ValueID name) {
        return name ? referenceTo(underlyingCellOf(m_code, name)) : Reference();
    };
    auto closureNamedBy = [&](Reference name) {
        Reference cell = cellNamedBy(name.id());
        return cell.isValue() && m_code.at(cell.id()).opcode == Opcode::NewClosure ? cell.id() : ValueID();
    };

    uint32_t numberOfNamedOperands = call.numberOfNamedOperands();
    Vector<Reference, 8> arguments(call.variadicOperands());
    Vector<Reference, 2> takenAsTheyAre;
    auto noteScopeThatCallTakes = [&](Reference cell, uint32_t position, FunctionID accessor, uint32_t capture, bool isHeldByClosure) {
        if (!isEnvironmentAllocation(cell) || !m_state.isFresh(cell))
            return;
        if (!operationOnlyBorrowsScope(call, cell, numberOfNamedOperands + position, isHeldByClosure) || !functionOnlyReadsAndWritesScopeAt(accessor, capture)) {
            takenAsTheyAre.append(cell);
            return;
        }
        size_t place = lent.findIf([&](const ScopeLentToCall& loan) { return loan.scope == cell; });
        if (place == notFound) {
            place = lent.size();
            lent.append(ScopeLentToCall { cell, { }, { }, { } });
        }
        const AccessesOfScopeByFunction& accesses = accessesOfScopeByFunction(accessor, capture);
        for (FieldReference field : accesses.fieldsAccessed)
            lent[place].fieldsAccessed.append(field.field);
        for (FieldReference field : accesses.fieldsStored)
            lent[place].fieldsStored.append(field.field);
    };
    for (uint32_t position = 0; position < arguments.size(); ++position) {
        if (!arguments[position] || !arguments[position].isValue())
            continue;
        noteScopeThatCallTakes(cellNamedBy(arguments[position].id()), position, call.function(), position, false);
        if (ValueID closure = closureNamedBy(arguments[position])) {
            uint32_t capture = 0;
            for (ValueID held : m_code.at(closure).capturesOperands())
                noteScopeThatCallTakes(cellNamedBy(held), position, m_code.at(closure).function(), capture++, true);
        }
    }
    lent.removeAllMatching([&](const ScopeLentToCall& loan) {
        return takenAsTheyAre.contains(loan.scope) || (!standsOnRareWay && !m_scopesWhoseFieldsHaveComeRound.contains(loan.scope.id().index()));
    });
    bool everyLoopAroundMayRunOtherCode = lent.isEmpty() || everyOpenLoopAround([&](uint32_t place) {
        return m_openLoops[place].effects.mayRunOtherCode();
    });
    if (!everyLoopAroundMayRunOtherCode)
        lent.shrink(0);

    Origin origin = call.origin();
    origin.isTailCall = false;
    for (ScopeLentToCall& loan : lent) {
        std::ranges::sort(loan.fieldsAccessed);
        loan.fieldsAccessed.shrink(std::ranges::unique(loan.fieldsAccessed).begin() - loan.fieldsAccessed.begin());
        std::ranges::sort(loan.fieldsStored);
        loan.fieldsStored.shrink(std::ranges::unique(loan.fieldsStored).begin() - loan.fieldsStored.begin());
        loan.copy = buildCopyOfScope(loan, origin);
    }
    lent.removeAllMatching([](const ScopeLentToCall& loan) {
        return !loan.copy;
    });
    if (lent.isEmpty() || isUnreachable())
        return;

    auto copyLentInPlaceOf = [&](ValueID name) {
        Reference cell = cellNamedBy(name);
        for (const ScopeLentToCall& loan : lent) {
            if (loan.scope == cell)
                return loan.copy;
        }
        return Reference();
    };
    for (Reference& argument : arguments) {
        if (!argument || !argument.isValue())
            continue;
        if (Reference copy = copyLentInPlaceOf(argument.id())) {
            argument = copy;
            continue;
        }
        ValueID closure = closureNamedBy(argument);
        if (!closure)
            continue;
        Vector<Reference, 4> captures;
        bool holdsCopy = false;
        for (ValueID capture : m_code.at(closure).capturesOperands()) {
            Reference copy = copyLentInPlaceOf(capture);
            holdsCopy |= !!copy;
            captures.append(copy ? copy : capture ? referenceTo(capture) : Reference());
        }
        if (!holdsCopy)
            continue;
        Draft overCopies(Opcode::NewClosure, origin);
        overCopies.standsForNoValueOfInput = true;
        overCopies.setFunction(m_code.at(closure).function());
        overCopies.setVariadicOperands(captures.span());
        chooseConservativeFields(overCopies);
        Reference made = build(WTF::move(overCopies));
        if (made && representationOf(made) != representationOf(argument))
            made = convert(made, representationOf(argument), origin);
        if (!made) {
            lent.shrink(0);
            return;
        }
        argument = made;
    }
    call.setVariadicOperands(arguments.span());
}

void Walker::takeBackScopesLentToCall(std::span<const ScopeLentToCall> lent, const Effects* ofCall, Origin origin)
{
    SetForScope originOfReads(m_origin, origin);
    for (const ScopeLentToCall& loan : lent) {
        LayoutID layout = m_code.at(loan.copy.id()).layout();
        for (FieldIndex index : loan.fieldsStored) {
            if (!plan().layout(layout).slots[index].hasStorage())
                continue;
            Draft read = draftCapturedVariableRead(loan.copy, { layout, index });
            if (ofCall && !ofCall->mayRunOtherCode() && !declaredEffectsOfOperation(program(), Opcode::GetEnvironmentField, read).reads.overlaps(ofCall->writes))
                continue;
            Reference value = build(WTF::move(read));
            if (!value)
                return;
            Draft store(Opcode::PutEnvironmentField, origin);
            store.setLayout(layout);
            store.setField(index);
            store.setEnvironmentOperand(loan.scope);
            store.setValueOperand(value);
            chooseConservativeFields(store);
            build(WTF::move(store));
            if (isUnreachable())
                return;
            noteEnvironmentField(loan.scope, { layout, index }, value);
        }
        if (!cellMadeByUnitInEnvironmentCells(*this, loan.copy))
            continue;
        Draft release(Opcode::ReleaseEnvironment, origin);
        release.setEnvironmentOperand(loan.copy);
        build(WTF::move(release));
        if (isUnreachable())
            return;
    }
}

Reference Walker::buildCallThatMayBorrowScopes(Draft&& call, const Effects* foundByCaller)
{
    if (!unitHasMadeScopes())
        return build(WTF::move(call), foundByCaller);
    Vector<ScopeLentToCall, 1> lent;
    lendCopiesOfScopesTo(call, lent);
    if (lent.isEmpty() || isUnreachable())
        return build(WTF::move(call), foundByCaller);
    bool unitGoesOnBehindCall = !unitReturnsAtOnce(call);
    Origin origin = call.origin();
    origin.isTailCall = false;
    Reference result = build(WTF::move(call), foundByCaller);
    if (result && unitGoesOnBehindCall)
        takeBackScopesLentToCall(lent.span(), foundByCaller, origin);
    return isUnreachable() ? Reference() : result;
}

bool Walker::mentionMayOnlyBorrowScopes(const BodyValue& operation, uint32_t operandIndex)
{
    Opcode opcode = operation.opcode();
    if (inputIdentityOpcodes.contains(opcode))
        return !operandIndex;
    if (opcode == Opcode::Call && operandIndex == OperandIndex::Call::callee)
        return true;
    uint32_t numberOfNamedOperands = numberOfNamedOperandsOf(opcode);
    if (operandIndex >= numberOfNamedOperands && opcode == Opcode::NewClosure)
        return functionOnlyReadsAndWritesScopeAt(operation.as<Opcode::NewClosure>().function(), operandIndex - numberOfNamedOperands);
    if (operandIndex >= numberOfNamedOperands && (opcode == Opcode::Call || opcode == Opcode::CallDirect))
        return true;
    const OperandInfo& info = operandInfoOf(opcode, std::min(operandIndex, numberOfNamedOperands));
    return info.protocols.isEmpty() && info.uses.containsOnly(OperandUses { OperandUse::Inspects });
}

void Walker::keepScopesAtMentions(Vector<ValueID, 4>& mentions, std::span<const ValueID> mentionsThatLeave, Vector<FreshObjectKeptAtHead, 2>& kept)
{
    std::ranges::sort(mentions);
    mentions.shrink(std::ranges::unique(mentions).begin() - mentions.begin());
    Vector<ValueID, 4> leaving(mentionsThatLeave);
    std::ranges::sort(leaving);
    Vector<Reference, 2> scopes;
    mentions.removeAllMatching([&](ValueID mention) {
        Reference reference = referenceOf(mention);
        scopes.shrink(0);
        listScopesThatValueNames(reference, scopes);
        if (!scopes.isEmpty() && !std::ranges::binary_search(leaving, mention))
            return false;
        if (reference && reference.isRecord())
            letLeave(reference);
        return true;
    });
    for (ValueID mention : mentions) {
        Reference reference = referenceOf(mention);
        Reference cell = referenceTo(underlyingCellOf(m_code, reference.id()));
        if (!isEnvironmentAllocation(cell))
            m_state.markEscaped(cell);
        scopes.shrink(0);
        listScopesThatValueNames(reference, scopes);
        for (Reference scope : scopes) {
            if (m_state.isFresh(scope))
                kept.append(FreshObjectKeptAtHead { mention, scope });
        }
    }
}

void Walker::handBackScopesThatBuildLetsLeave(std::span<const ValueID> mentionsThatLeave)
{
    Vector<Reference, 2> scopes;
    for (ValueID mention : mentionsThatLeave) {
        if (m_inheritedEscapingUses.contains(mention))
            continue;
        if (mention == everyFreshObject) {
            m_inheritedEscapingUses.append(mention);
            continue;
        }
        scopes.shrink(0);
        listScopesThatValueNames(referenceOf(mention), scopes);
        if (!scopes.isEmpty())
            m_inheritedEscapingUses.append(mention);
    }
}

void Walker::handScopesKeptAtHeadToPreview(const BodyIndex& index, BlockID inputHead, const StoresIntoScopes& stores, std::span<const FreshObjectKeptAtHead> kept, LoopPreview& preview)
{
    Vector<Reference, 2>& scopes = preview.scopesThatTurnsOnlyBorrow;
    scopes.shrink(0);
    preview.slots = stores.slots;
    preview.counters = stores.counters;
    for (const FreshObjectKeptAtHead& object : kept) {
        if (isEnvironmentAllocation(object.object))
            scopes.append(object.object);
    }
    if (scopes.isEmpty())
        return;
    std::ranges::sort(scopes, { }, [](Reference scope) { return scope.id(); });
    scopes.shrink(std::ranges::unique(scopes).begin() - scopes.begin());
    preview.fieldsOfScopesThatTurnsStore = stores.fields;
    preview.fieldsOfScopesThatCallsInTurnsStore = stores.fieldsThatCallsStore;
    if (preview.effects.maySuspend)
        return;
    if (stores.everyFieldIsTakenForStored) {
        for (Reference scope : scopes) {
            LayoutID layout = m_code.at(scope.id()).layout();
            uint32_t numberOfFields = static_cast<uint32_t>(program().at(layout).fields.size());
            for (uint32_t number = 0; number < numberOfFields; ++number) {
                FieldOfScope field { scope, { layout, number }, TypeID(), Representation::JSValue, { }, { m_state.environmentField(scope, { layout, number }) }, { } };
                if (field.values[0] && mayHandFieldOfScopeToBlock(field))
                    preview.fieldsOfScopesThatTurnsStore.append(field.field);
            }
        }
    }
    if (stores.areKnown || !index.opcodes().contains(Opcode::PutEnvironmentField)) {
        std::ranges::sort(preview.fieldsOfScopesThatTurnsStore);
        preview.fieldsOfScopesThatTurnsStore.shrink(std::ranges::unique(preview.fieldsOfScopesThatTurnsStore).begin() - preview.fieldsOfScopesThatTurnsStore.begin());
        return;
    }
    for (BlockID member : index.membersOf(index.loopOf(inputHead))) {
        if (!index.isReachable(member))
            continue;
        for (BodyValue operation : index.block(member).values()) {
            if (operation.opcode() != Opcode::PutEnvironmentField)
                continue;
            FieldReference place { operation.layout(), operation.field() };
            for (Reference scope : scopes) {
                if (m_code.at(scope.id()).layout() != place.layout)
                    continue;
                FieldOfScope field { scope, place, TypeID(), Representation::JSValue, { }, { m_state.environmentField(scope, place) }, { } };
                if (field.values[0] && mayHandFieldOfScopeToBlock(field))
                    preview.fieldsOfScopesThatTurnsStore.append(place);
            }
        }
    }
    std::ranges::sort(preview.fieldsOfScopesThatTurnsStore);
    preview.fieldsOfScopesThatTurnsStore.shrink(std::ranges::unique(preview.fieldsOfScopesThatTurnsStore).begin() - preview.fieldsOfScopesThatTurnsStore.begin());
}

bool Walker::learnWhatTurnsStoreIntoScopes(LoopSummary& summary, const TrialVisit& trialVisit, bool walkIsWhole)
{
    StoresIntoScopes& stores = summary.storesIntoScopes;
    bool tookEveryField = stores.everyFieldIsTakenForStored;
    bool wereKnown = stores.areKnown && !tookEveryField;
    if (!trialVisit.keepsScopesAtHead || (!wereKnown && !walkIsWhole))
        return false;
    stores.everyFieldIsTakenForStored = false;
    stores.everyFieldWasTakenForStored |= tookEveryField;
    auto add = [](Vector<FieldReference, 4>& fields, const Vector<FieldReference, 4>& found) {
        size_t numberBefore = fields.size();
        fields.appendVector(found);
        std::ranges::sort(fields);
        fields.shrink(std::ranges::unique(fields).begin() - fields.begin());
        return fields.size() != numberBefore;
    };
    stores.areKnown = true;
    bool hasFoundCounter = false;
    for (const CapturedCounter& counter : trialVisit.counters) {
        bool isPosition = counter.parameter && trialVisit.positions.containsIf([&](const auto& position) { return position.first == counter.parameter; });
        size_t place = stores.counters.findIf([&](const CapturedCounter& found) { return found.field == counter.field; });
        if (place != notFound)
            stores.counters[place].hasLeftIntegers |= counter.hasLeftIntegers;
        else if (isPosition) {
            stores.counters.append(counter);
            hasFoundCounter |= !counter.hasLeftIntegers;
        }
    }
    bool hasLearnedMore = add(stores.fields, trialVisit.fieldsOfScopesStored);
    hasLearnedMore |= add(stores.fieldsThatCallsStore, trialVisit.fieldsOfScopesEnded);
    if (m_inheritedSummaryHeader == trialVisit.inputHead && m_inheritedSummaryFrame == trialVisit.frameIndex)
        add(m_inheritedScopeStores.fields, stores.fields);
    if (wereKnown) {
        stores.everyFieldIsTakenForStored = hasLearnedMore && !stores.everyFieldWasTakenForStored;
        return hasLearnedMore || hasFoundCounter;
    }
    if (hasFoundCounter)
        return true;
    BlockID head = inputBlock(trialVisit.frameIndex, trialVisit.inputHead).head;
    auto carried = head ? m_fieldsOfScopesOfHeads.find(head.index()) : m_fieldsOfScopesOfHeads.end();
    return carried != m_fieldsOfScopesOfHeads.end() && std::ranges::any_of(carried->value, [&](const FieldOfScope& field) {
        if (field.variable || !isEnvironmentAllocation(field.scope))
            return false;
        return !std::ranges::binary_search(stores.fields, field.field) || std::ranges::binary_search(stores.fieldsThatCallsStore, field.field);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
