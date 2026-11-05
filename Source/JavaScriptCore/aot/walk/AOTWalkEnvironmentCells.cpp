/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

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
    return planned.cellsAreHandedBack && !planned.liesOnlyInFrames;
}

static ValueID recordBehindNamesInEnvironmentCells(const Code& code, Reference reference)
{
    if (!reference || !reference.isValue())
        return { };
    ValueID root = rootOfCellInSweptCode(code, reference.id());
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
        Reference handedOver = walker.referenceBoundTo(entry.parameter(capture));
        if (!handedOver || !handedOver.isValue() || !layoutHandedBackInEnvironmentCells(walker, handedOver.id()))
            continue;
        if (mayBeNothingInEnvironmentCells(walker, handedOver.id())) {
            walker.fail("what may be no environment is kept, and nobody can tell whether it has a byte to set"_s);
            return;
        }
        if (!buildMarkInEnvironmentCells(walker, handedOver, walker.originOfOperation()))
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
    uint32_t copy = allocation.origin().inlineFrame;
    for (Walker::EnvironmentCellsOfUnit::Name& noted : cells.names) {
        if (noted.body == &input && noted.copy == copy && noted.name == name) {
            noted.hasSeveralRecords = true;
            return;
        }
    }
    BlockID making = input.blockOf(name);
    if (input.loopOf(making) || input.block(making).hasHandler() || input.hasIrreducibleFlow() || walker.program().at(walker.currentFunction()).isCoroutine())
        return;
    cells.names.append({ &input, copy, name, making, false });
}

void handBackEnvironmentsAtReturn(Walker& walker, BlockID block)
{
    if (walker.isUnreachable())
        return;
    const BodyIndex& input = walker.currentBodyIndex();
    Origin origin = walker.originOfOperation();
    origin.isTailCall = false;
    const auto& names = walker.environmentCellsOfUnit().names;
    for (size_t index = 0; index < names.size(); ++index) {
        const Walker::EnvironmentCellsOfUnit::Name& name = names[index];
        if (name.body != &input || name.copy != origin.inlineFrame || name.hasSeveralRecords || !input.dominates(name.making, block))
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

bool Walker::isScopeThatUnitMakes(Reference cell) const
{
    if (!cell || !cell.isValue())
        return false;
    const Operation& made = m_code.at(cell.id());
    return made.opcode == Opcode::NewObject && program().at(made.layout()).isEnvironment();
}

bool Walker::namesFreshCell(const WalkState& state, Reference value) const
{
    return value && value.isValue() && state.isFresh(referenceTo(rootOfCellInSweptCode(m_code, value.id())));
}

bool Walker::mayAssumeTypeOfRead(Reference environment, FieldReference field, Reference value, TypeID typeOfRead)
{
    TypeID own = ownTypeOf(value);
    Representation form = representationOf(value);
    if (!own || !value.isValue() || !hasRegister(form))
        return false;
    bool mayBeWhatAllocationLeft = (m_types.at(own).primitives & Primitive::Undefined) && !(m_types.at(typeOfRead).primitives & Primitive::Undefined);
    if (mayBeWhatAllocationLeft && !m_state.isPastDeadZone(environment, field))
        return false;
    TypeID both = m_types.intersectionOf(own, typeOfRead);
    if (isNeverType(both) || !isLegalRepresentation(m_types, both, form))
        return false;
    return !numberRepresentations.contains(form) || isShown(value, typeOfRead);
}

Reference Walker::assumeTypeOfRead(Reference value, TypeID typeOfRead, Origin origin)
{
    origin.isTailCall = false;
    Draft assumed(Opcode::AssumeType, origin);
    assumed.setValueOperand(value);
    assumed.setProvenType(typeOfRead);
    assumed.setSource(ProofSource::SlotOfScope);
    assumed.representation = representationOf(value);
    return build(WTF::move(assumed));
}

Reference Walker::typeAnswerOfStateAsRead(const BodyValue& read, Reference environment, FieldReference field, Reference held)
{
    if (!unitHasMadeScopes() || !held.isValue() || !isScopeThatUnitMakes(environment))
        return held;
    TypeID own = ownTypeOf(held);
    TypeID linked = positions().environmentField(TypeLevel::Proven, field.layout, field.field);
    if (!own || !linked || m_types.isSubset(own, linked))
        return held;
    Draft draft = draftOf(read);
    chooseConservativeFields(draft);
    TypeID typeOfRead = resultTypeOf(draft);
    if (!typeOfRead || m_types.isSubset(own, typeOfRead) || !mayAssumeTypeOfRead(environment, field, held, typeOfRead))
        return held;
    Reference assumed = assumeTypeOfRead(held, typeOfRead, originOf(read));
    return assumed ? assumed : held;
}

bool Walker::addFieldsThatClosureStores(Reference closure, Vector<FieldReference, 4>& stored)
{
    if (!closure.isValue())
        return true;
    const Operation& made = m_code.at(rootOfCellInSweptCode(m_code, closure.id()));
    if (made.opcode != Opcode::NewClosure)
        return true;
    bool hasFoundEveryStore = true;
    uint32_t position = 0;
    for (ValueID capture : made.capturesOperands()) {
        if (capture && isScopeThatUnitMakes(referenceTo(rootOfCellInSweptCode(m_code, capture))))
            hasFoundEveryStore &= addFieldsThatFunctionStoresIntoCapture(made.function(), position, stored);
        ++position;
    }
    return hasFoundEveryStore;
}

bool Walker::addFieldsThatFunctionStoresIntoCapture(FunctionID function, uint32_t capture, Vector<FieldReference, 4>& stored)
{
    const AccessesOfScopeByFunction& accesses = accessesOfScopeByFunction(function, capture);
    for (FieldReference field : accesses.fieldsStored)
        stored.appendIfNotContains(field);
    return accesses.areAllItsMentions;
}

void Walker::listScopesThatValueNames(Reference value, Vector<Reference, 2>& scopes) const
{
    if (!value.isValue())
        return;
    Reference cell = referenceTo(rootOfCellInSweptCode(m_code, value.id()));
    if (isScopeThatUnitMakes(cell))
        scopes.append(cell);
    if (!cell.isValue() || m_code.at(cell.id()).opcode != Opcode::NewClosure)
        return;
    for (ValueID capture : m_code.at(cell.id()).capturesOperands()) {
        Reference held = capture ? referenceTo(rootOfCellInSweptCode(m_code, capture)) : Reference();
        if (isScopeThatUnitMakes(held))
            scopes.append(held);
    }
}

void Walker::findFieldsOfScopesThatMeet(std::span<const WalkState* const> states, Vector<FieldOfScope, 4>& found)
{
    if (states.size() < 2)
        return;
    Vector<Representation, 4> forms;
    states[0]->forEachEnvironmentFieldNotSharedWithAll(states, [&](Reference environment, FieldReference place, Reference first) {
        if (!isScopeThatUnitMakes(environment))
            return;
        FieldOfScope field { environment, place, TypeID(), Representation::JSValue, { }, { }, { } };
        bool allBringOneValue = true;
        forms.shrink(0);
        for (const WalkState* state : states) {
            Reference value = state->environmentField(environment, place);
            TypeID type = ownTypeOf(value);
            if (!type || (m_types.at(type).primitives & Primitive::Absent) || representationOf(value) == Representation::Void || namesFreshCell(*state, value) || isClosureLeftUnmade(*this, value))
                return;
            allBringOneValue &= value == first;
            field.type = field.type ? joinTypes(field.type, type) : type;
            field.values.append(value);
            forms.append(representationOf(value));
        }
        if (allBringOneValue)
            return;
        field.form = formOfParameter(field.type, forms.span());
        found.append(WTF::move(field));
    });
}

Vector<FieldReference, 2> Walker::fieldsAtWhoseReadsLooksCountOnGuesses(std::span<const GuessCountedOnByLooks> guesses) const
{
    Vector<FieldReference, 2> fields;
    const BodyIndex& index = currentBodyIndex();
    for (const GuessCountedOnByLooks& guess : guesses) {
        ValueID guessed = index.valueBehindCasts(guess.operand);
        if (guessed && !index.isParameter(guessed) && index.opcodeOf(guessed) == Opcode::CheckInitialized)
            guessed = index.valueBehindCasts(index.value(guessed).operand(0));
        if (!guessed || index.isParameter(guessed) || index.opcodeOf(guessed) != Opcode::GetEnvironmentField)
            continue;
        auto read = index.value(guessed).as<Opcode::GetEnvironmentField>();
        fields.append(FieldReference { read.layout(), read.field() });
    }
    return fields;
}

void Walker::findFieldsOfScopesInFrontOfLoop(std::span<const FieldReference> guessedAtReads, const LoopPreview& look, Vector<FieldOfScope, 4>& known)
{
    for (Reference scope : look.scopesThatTurnsOnlyBorrow) {
        LayoutID layout = m_code.at(scope.id()).layout();
        for (FieldReference place : look.fieldsOfScopesThatTurnsStore) {
            bool mayComeRound = place.layout == layout && !look.fieldsOfScopesThatCallsInTurnsStore.contains(place) && !std::ranges::contains(guessedAtReads, place);
            if (Reference value = mayComeRound ? m_state.environmentField(scope, place) : Reference())
                known.append(FieldOfScope { scope, place, TypeID(), Representation::JSValue, { }, { value }, { } });
        }
    }
}

Draft Walker::draftOfReadOfFieldThatComesRound(Reference scope, FieldReference field)
{
    Draft read(Opcode::GetEnvironmentField, m_origin);
    read.setEnvironmentOperand(scope);
    read.setLayout(field.layout);
    read.setField(field.field);
    chooseConservativeFields(read);
    read.setMayPrecedeItsStore(false);
    read.setFollowsItsStore(true);
    return read;
}

bool Walker::isFitForParameterOfField(const FieldOfScope& field, Reference value)
{
    TypeID type = ownTypeOf(value);
    return type && m_types.isSubset(type, field.type) && representationOf(value) != Representation::Void && !isClosureLeftUnmade(*this, value) && (!field.shown || isShown(value, field.shown));
}

Reference Walker::fitForParameterOfField(const FieldOfScope& field, Reference known)
{
    if (isFitForParameterOfField(field, known))
        return known;
    if (!mayAssumeTypeOfRead(field.scope, field.field, known, field.type))
        return { };
    Reference assumed = assumeTypeOfRead(known, field.type, m_origin);
    return isFitForParameterOfField(field, assumed) ? assumed : Reference();
}

bool Walker::mayHandFieldOfScopeToBlock(FieldOfScope& field)
{
    const Vector<Field>& fields = program().at(field.field.layout).fields;
    Reference entering = field.values[0];
    if (field.field.field >= fields.size() || fields[field.field.field].declared.isEstablished() || namesFreshCell(m_state, entering))
        return false;
    field.type = resultTypeOf(draftOfReadOfFieldThatComesRound(field.scope, field.field));
    if (!field.type || !m_types.at(field.type).isJavaScriptValue())
        return false;
    field.form = formOfParameter(field.type, { });
    if (field.form == Representation::JSValue && isEnforced(ProofSource::SlotOfScope))
        field.shown = typeThatUnitMayTestFor(m_types.testablePartOf(field.type), true, true);
    return isFitForParameterOfField(field, entering) || mayAssumeTypeOfRead(field.scope, field.field, entering, field.type);
}

void Walker::noteWhatParameterOfFieldIsShownToBe(const FieldOfScope& field)
{
    if (field.shown)
        m_typesAsked.set(field.parameter.id().index(), field.shown);
}

void Walker::keepFieldsOfScopesThatLoopWrites(Vector<FieldOfScope, 4>& known)
{
    known.removeAllMatching([&](FieldOfScope& field) {
        if (m_state.environmentField(field.scope, field.field) || !mayHandFieldOfScopeToBlock(field))
            return true;
        field.values[0] = fitForParameterOfField(field, field.values[0]);
        return !field.values[0];
    });
    for (const FieldOfScope& field : known)
        m_scopesWhoseFieldsHaveComeRound.add(field.scope.id().index());
}

bool Walker::bringFieldsOfScopesRound(EdgeID& edge, BlockID head, Vector<Reference, 8>& arguments)
{
    auto fields = m_fieldsOfScopesOfHeads.find(head.index());
    if (fields == m_fieldsOfScopesOfHeads.end())
        return true;
    auto comesRoundAsKnown = [&](const FieldOfScope& field) {
        return isFitForParameterOfField(field, m_state.environmentField(field.scope, field.field));
    };
    if (edge && !std::ranges::all_of(fields->value, comesRoundAsKnown)) {
        BlockID onEdge = m_code.addBlock(BlockRole::Ordinary, m_code.at(head).frequency);
        m_code.setLoopMarks(onEdge, m_code.at(head).isInLoop, false);
        m_code.closeEdge(edge, onEdge, { });
        m_block = onEdge;
        edge = { };
    }
    for (const FieldOfScope& field : fields->value) {
        Reference value = fitForParameterOfField(field, m_state.environmentField(field.scope, field.field));
        if (!value)
            value = build(draftOfReadOfFieldThatComesRound(field.scope, field.field));
        if (!value) {
            if (!m_hasFailed && !isUnreachable())
                fail("a field of a scope does not come round to the head of its loop"_s);
            m_block = { };
            return false;
        }
        arguments.append(value);
    }
    return true;
}

void Walker::takeFieldsOfScopesIntoClosedCopy(BlockID entry, const LoopPreview& look)
{
    if (look.scopesThatTurnsOnlyBorrow.isEmpty())
        return;
    Vector<FieldOfScope, 4> fields;
    findFieldsOfScopesInFrontOfLoop({ }, look, fields);
    fields.removeAllMatching([&](FieldOfScope& field) {
        return !mayHandFieldOfScopeToBlock(field);
    });
    if (fields.isEmpty())
        return;
    for (FieldOfScope& field : fields) {
        field.parameter = addParameter(entry, field.type, field.form);
        noteWhatParameterOfFieldIsShownToBe(field);
    }
    m_fieldsOfScopesOfHeads.add(entry.index(), Vector<FieldOfScope> { }).iterator->value.append(fields.span());
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
    return fields == m_fieldsOfScopesOfHeads.end() ? 0 : fields->value.size();
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
    if (!isScopeThatUnitMakes(scope) || !unitKeepsScopesToItself())
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

void Walker::letLeaveAtMentionThatOnlyBorrowsScopes(Reference reference)
{
    if (!reference.isValue())
        return;
    ValueID cell = rootOfCellInSweptCode(m_code, reference.id());
    if (!m_scopesThatClosuresHold.contains(cell.index()))
        m_state.markEscaped(referenceTo(cell));
}

void Walker::noteThatWalkLetsScopesNamedLeave(Reference reference)
{
    if (m_openLoops.isEmpty())
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
    if (m_hasFailed || m_openLoops.isEmpty() || !scope.isValue() || !m_scopesThatClosuresHold.contains(scope.id().index()) || !m_state.isFresh(scope))
        return;
    bool isMadeInEveryLoopAround = everyOpenLoopAround([&](uint32_t place) {
        return !standsInFrontOfLoop(place, scope);
    });
    if (isMadeInEveryLoopAround)
        return;
    if (std::exchange(m_loopsLetScopesLeave, true) || m_inputs.options.verifiesCode) {
        fail("a scope that the unit keeps to itself leaves in a turn of a loop whose look has not seen it leave"_s);
        return;
    }
    dataLogLn("AOT: f", unit().function.index(), " ", nameOf(unit().role), " is walked again: in f", currentFunction().index(), ", where a ", nameOf(m_opcodeBeingWalked), " is written in a loop, a scope leaves that the looks took to be borrowed, and every loop of the unit now lets the scopes that it names leave");
    giveUpWalkOfUnit();
}

void Walker::endFieldsOfScopesThatTurnsStore(const LoopPreview& look)
{
    Vector<Reference, 2> scopes;
    for (Reference scope : look.scopesThatTurnsOnlyBorrow) {
        if (m_scopesThatClosuresHold.contains(scope.id().index()) && m_state.isFresh(scope))
            scopes.append(scope);
    }
    if (scopes.isEmpty() || look.fieldsOfScopesThatTurnsStore.isEmpty())
        return;
    Effects stores = Effects::none();
    for (FieldReference field : look.fieldsOfScopesThatTurnsStore)
        stores.writes.add(program(), MemoryLocation::environmentField(field.layout, field.field));
    for (Reference scope : scopes)
        m_state.markEscaped(scope);
    m_state.invalidate(stores);
    for (Reference scope : scopes)
        m_state.noteFresh(scope);
}

void Walker::enterClosedCopy(BlockID entry, const LoopPreview& look)
{
    m_block = entry;
    if (!has(Mechanism::EscapeAnalysis) || !look.hasLetMentionsLeave || look.effects.maySuspend || !m_state.holdsFreshObjects())
        m_state.clear();
    else {
        endFieldsOfScopesThatTurnsStore(look);
        m_state.invalidate(look.effects);
        m_state.forgetAllButFreshObjects();
    }
    noteFieldsOfScopesThatBlockTakes(entry);
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

bool Walker::copyOfScopeTakesStoresOutOfLoop(Reference scope) const
{
    if (!m_scopesWhoseFieldsHaveComeRound.contains(scope.id().index()) || m_scopesCopiedOnWaysThatAreNotRare.contains(scope.id().index()))
        return false;
    return std::ranges::none_of(m_openLoops, [&](const OpenLoop& open) {
        auto fields = m_fieldsOfScopesOfHeads.find(open.head.index());
        return fields != m_fieldsOfScopesOfHeads.end() && fields->value.containsIf([&](const FieldOfScope& field) {
            return field.scope == scope;
        });
    });
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
        return name ? referenceTo(rootOfCellInSweptCode(m_code, name)) : Reference();
    };
    auto closureNamedBy = [&](Reference name) {
        Reference cell = cellNamedBy(name.id());
        return cell.isValue() && m_code.at(cell.id()).opcode == Opcode::NewClosure ? cell.id() : ValueID();
    };

    uint32_t numberOfNamedOperands = call.numberOfNamedOperands();
    Vector<Reference, 8> arguments(call.variadicOperands());
    Vector<Reference, 2> takenAsTheyAre;
    auto noteScopeThatCallTakes = [&](Reference cell, uint32_t position, FunctionID accessor, uint32_t capture, bool isHeldByClosure) {
        if (!isScopeThatUnitMakes(cell) || !m_state.isFresh(cell))
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
            lent[place].fieldsAccessed.appendIfNotContains(field.field);
        for (FieldReference field : accesses.fieldsStored)
            lent[place].fieldsStored.appendIfNotContains(field.field);
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
        return takenAsTheyAre.contains(loan.scope) || (!standsOnRareWay && !copyOfScopeTakesStoresOutOfLoop(loan.scope));
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
        std::ranges::sort(loan.fieldsStored);
        loan.copy = buildCopyOfScope(loan, origin);
    }
    lent.removeAllMatching([](const ScopeLentToCall& loan) {
        return !loan.copy;
    });
    if (lent.isEmpty() || isUnreachable())
        return;
    if (!standsOnRareWay) {
        for (const ScopeLentToCall& loan : lent)
            m_scopesCopiedOnWaysThatAreNotRare.add(loan.scope.id().index());
    }

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
        if (!made)
            return;
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
            Draft read = draftOfReadOfFieldThatComesRound(loan.copy, { layout, index });
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

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
