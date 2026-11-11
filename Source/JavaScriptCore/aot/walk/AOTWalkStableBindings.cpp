/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTDeclaredFunctionsInScopes.h"
#include "AOTIRProgram.h"
#include "AOTMemoryLocation.h"
#include "AOTOpcodeSets.h"
#include "AOTSweep.h"
#include "AOTWalkKnown.h"

namespace JSC { namespace AOT { namespace Typed {

Reference findCaptureAtHand(Walker& walker, FunctionID callee, uint32_t capture, Reference closure)
{
    if (!closure || !closure.isValue() || !walker.has(Mechanism::DirectCalls))
        return { };
    const Program& program = walker.program();
    if (capture >= program.at(callee).captures.size())
        return { };
    LayoutID scope = scopeThatCaptureTakes(program, callee, capture);
    if (!scope)
        return { };

    const Code& code = walker.code();
    const Operation& read = code.at(underlyingCellOf(code, closure.id()));
    if (read.opcode != Opcode::GetEnvironmentField || read.layout() != scope)
        return { };
    if (functionDeclaredInScopeSlot(program, scope, read.field()) != callee)
        return { };
    ValueID holder = read.environmentOperand();
    if (!program.moduleInstances.isEmpty() && !code.isConstant(holder) && code.at(holder).opcode == Opcode::Unbox)
        holder = code.at(holder).operand(0);
    return code.isConstant(holder) ? Reference::constant(holder) : Reference::value(holder);
}

enum class DefinitionOfStableVariable : uint8_t { IsRunByModule, IsPutOff };

static bool variableStaysAsFilled(const Walker& walker, VariableID id, DefinitionOfStableVariable definition = DefinitionOfStableVariable::IsRunByModule)
{
    const Program& program = walker.program();
    if (id.index() >= program.variables.size())
        return false;
    const Variable& variable = program.at(id);
    if (variable.owner || !variable.module || !variable.flags.contains(VariableFlags::IsImmutable))
        return false;
    if (variable.flags.containsAny({ VariableFlags::IsReplaceable, VariableFlags::HoldsMock, VariableFlags::OriginalMayRun }))
        return false;
    if (!loaderEntersBodyAtMostOnce(program.at(variable.module)))
        return false;
    const Plan& plan = walker.plan();
    if (plan.runTimeMayFill(id))
        return false;
    return !!plan.deferredDefinitions().findDeferred(id) == (definition == DefinitionOfStableVariable::IsPutOff);
}

bool Walker::framesKeepFields() const
{
    const Frame& last = m_frames.last();
    if (!last.keepsFields) {
        last.keepsFields = std::ranges::none_of(m_frames, [&](const Frame& frame) {
            const Function& function = program().at(frame.function);
            return function.isCoroutine() || function.constructedClass || function.kind == FunctionKind::Initializer;
        });
    }
    return *last.keepsFields;
}

bool Walker::isStableFieldOf(LayoutSetID receivers, PropertyKeyID key, bool holdsDeclaredType) const
{
    auto isStableIn = [&](LayoutID member) {
        std::optional<FieldIndex> field = plan().slots().findField(program(), member, key);
        if (!field)
            return false;
        const PlacedSlot& slot = plan().slots().slot(member, *field);
        return holdsDeclaredType ? slot.isStableWithoutNullish : slot.isStable;
    };
    std::span<const LayoutID> members = membersOf(m_types, receivers);
    if (members.size() <= 1)
        return !members.empty() && isStableIn(members.front());
    if (m_types.keeps() != ArenaKeeps::UntilReset)
        return std::ranges::all_of(members, isStableIn);
    uint8_t question = static_cast<uint8_t>(holdsDeclaredType ? AnswerKeptInArena::DeclaredFieldOfLayoutsIsStable : AnswerKeptInArena::FieldOfLayoutsIsStable);
    TypeArena::SetOfRound round = m_types.setOfRound(receivers);
    if (std::optional<TypeArena::WordsOfRound> kept = m_types.rememberedRound(question, 1, round, key.bits()))
        return (*kept)[0];
    bool isStable = std::ranges::all_of(members, isStableIn);
    m_types.rememberRound(question, 1, 0, round, key.bits(), { isStable });
    return isStable;
}

bool Walker::readsStableField(const Draft& read) const
{
    if (read.level() != KnowledgeLevel::Proven || read.receiverMayBeInBirth() || !isStableFieldOf(read.receivers(), read.key(), read.holdsDeclaredType()) || testsItsOwnGuess(read))
        return false;
    Reference object = read.objectOperand();
    if (!object || !object.isValue() || !framesKeepFields())
        return false;
    const Operation& holder = m_code.at(underlyingCellOf(m_code, object.id()));
    if (resultRuleOf(holder.opcode) == ResultRule::Allocation || interpretedOf(holder.opcode) == Interpretation::Allocation)
        return false;
    return holder.opcode != Opcode::Parameter || (holder.block() && m_code.at(holder.block()).isEntry);
}

bool numberReadOutOfVariableStays(const Walker& walker, VariableID variable)
{
    return variableStaysAsFilled(walker, variable);
}

static bool scopeSlotStaysAsFilled(const Walker& walker, FieldReference field)
{
    const Layout& layout = walker.program().at(field.layout);
    return layout.isEnvironment() && field.field < layout.fields.size() && layout.fields[field.field].flags.contains(FieldFlags::IsImmutable);
}

static bool keepsNotesOfStableBindings(const Walker& walker)
{
    return walker.has(Mechanism::Folding) && walker.state().remembersPlaces();
}

Reference Walker::valueOfStableField(const Draft& read, TypeID proven) const
{
    if (!keepsNotesOfStableBindings(*this))
        return { };
    std::optional<uint64_t> noted = findKnown(*this, KnownNote::ValueKeptByPlace, Reference::value(underlyingCellOf(m_code, read.objectOperand().id())), { }, read.key().index());
    Reference kept = noted ? Reference::fromBits(static_cast<uint32_t>(*noted)) : Reference();
    return kept && m_types.isSubset(typeOf(kept), proven) ? kept : Reference();
}

void Walker::noteStableField(const Draft& read, Reference value)
{
    if (keepsNotesOfStableBindings(*this))
        noteKnown(*this, KnownNote::ValueKeptByPlace, Reference::value(underlyingCellOf(m_code, read.objectOperand().id())), { }, read.key().index(), LocationMasks(), value.bits(), true);
}

void Walker::noteStableProof(Reference object, PropertyKeyID key, Reference proof)
{
    if (!keepsNotesOfStableBindings(*this))
        return;
    Reference holder = Reference::value(underlyingCellOf(m_code, object.id()));
    if (findKnown(*this, KnownNote::ValueKeptByPlace, holder, { }, key.index()))
        noteKnown(*this, KnownNote::ValueKeptByPlace, holder, { }, key.index(), LocationMasks(), proof.bits(), true);
}

static ALWAYS_INLINE bool isValueOfFilledBinding(Walker& walker, Reference value, bool hasDeadZone)
{
    TypeID type = value ? walker.typeOf(value) : TypeID();
    uint16_t unfilledPrimitives = Primitive::Absent | Primitive::Unspecified | (hasDeadZone ? 0 : Primitive::Undefined);
    return type && !(walker.types().at(type).primitives & unfilledPrimitives);
}

static bool hasDeadZone(const Walker& walker, VariableID variable)
{
    return walker.program().at(variable).initialization == VariableInitialization::Uninitialized;
}

static bool hasDeadZone(const Walker& walker, FieldReference field)
{
    TypeID written = walker.program().at(field.layout).fields[field.field].declared.written();
    return written && (walker.program().at(written).primitives & Primitive::Absent);
}

void noteValueOfStableBinding(Walker& walker, VariableID variable, Reference value)
{
    if (keepsNotesOfStableBindings(walker) && variableStaysAsFilled(walker, variable) && isValueOfFilledBinding(walker, value, hasDeadZone(walker, variable)))
        noteKnown(walker, KnownNote::ValueKeptByPlace, { }, { }, variable.index(), LocationMasks(), value.bits(), true);
}

void noteValueOfStableBinding(Walker& walker, Reference environment, FieldReference field, Reference value)
{
    if (environment && keepsNotesOfStableBindings(walker) && scopeSlotStaysAsFilled(walker, field) && isValueOfFilledBinding(walker, value, hasDeadZone(walker, field)))
        noteKnown(walker, KnownNote::ValueKeptByPlace, environment, { }, field.field, LocationMasks(), value.bits(), true);
}

void noteObjectOfVariablePutOff(Walker& walker, VariableID variable, Reference object)
{
    if (keepsNotesOfStableBindings(walker) && variableStaysAsFilled(walker, variable, DefinitionOfStableVariable::IsPutOff) && isValueOfFilledBinding(walker, object, hasDeadZone(walker, variable)))
        noteKnown(walker, KnownNote::ValueKeptByPlace, { }, { }, variable.index(), LocationMasks(), object.bits(), true);
}

bool objectOfVariablePutOffStays(const Walker& walker, VariableID variable)
{
    return keepsNotesOfStableBindings(walker) && isVariableFilledAtFirstRead(walker.plan(), variable) && variableStaysAsFilled(walker, variable, DefinitionOfStableVariable::IsPutOff);
}

Reference objectOfVariablePutOff(Walker& walker, VariableID variable)
{
    if (!keepsNotesOfStableBindings(walker) || !variableStaysAsFilled(walker, variable, DefinitionOfStableVariable::IsPutOff))
        return { };
    std::optional<uint64_t> noted = findKnown(walker, KnownNote::ValueKeptByPlace, { }, { }, variable.index());
    return noted ? Reference::fromBits(static_cast<uint32_t>(*noted)) : Reference();
}

Reference valueOfStableVariable(Walker& walker, VariableID variable, TypeID widest)
{
    if (!keepsNotesOfStableBindings(walker) || !variableStaysAsFilled(walker, variable))
        return { };
    std::optional<uint64_t> noted = findKnown(walker, KnownNote::ValueKeptByPlace, { }, { }, variable.index());
    if (!noted)
        return { };
    Reference kept = Reference::fromBits(static_cast<uint32_t>(*noted));
    TypeID type = walker.typeOf(kept);
    return widest && type && walker.types().isSubset(type, widest) ? kept : Reference();
}

static bool isLinkRead(const Program& program, const BodyIndex& index, ValueID scope)
{
    if (program.moduleInstances.isEmpty() || index.isParameter(scope) || index.opcodeOf(scope) != Opcode::GetEnvironmentField)
        return false;
    auto read = index.value(scope).as<Opcode::GetEnvironmentField>();
    FieldReference field { read.layout(), read.field() };
    return program.isInstanceLink(field) && program.isInstanceConstant(field);
}

bool isLoopInvariantScope(const Program& program, const BodyIndex& index, ValueID scope, uint32_t firstValueOfLoop)
{
    return scope.index() < firstValueOfLoop || (isLinkRead(program, index, scope) && isLoopInvariantScope(program, index, index.value(scope).environmentOperand(), firstValueOfLoop));
}

bool isSameScope(const Program& program, const BodyIndex& index, ValueID one, ValueID other)
{
    if (one == other)
        return true;
    if (!isLinkRead(program, index, one) || !isLinkRead(program, index, other))
        return false;
    BodyValue link = index.value(one);
    BodyValue otherLink = index.value(other);
    return link.layout() == otherLink.layout() && link.field() == otherLink.field() && isSameScope(program, index, link.environmentOperand(), otherLink.environmentOperand());
}

Reference loopInvariantScope(Walker& walker, const BodyIndex& index, ValueID scope, uint32_t firstValueOfLoop)
{
    if (scope.index() < firstValueOfLoop)
        return walker.referenceOf(scope);
    if (!keepsNotesOfStableBindings(walker) || !isLinkRead(walker.program(), index, scope))
        return { };
    BodyValue link = index.value(scope);
    Reference environment = loopInvariantScope(walker, index, link.environmentOperand(), firstValueOfLoop);
    std::optional<uint64_t> noted = environment ? findKnown(walker, KnownNote::ValueKeptByPlace, environment, { }, link.field()) : std::nullopt;
    return noted ? Reference::fromBits(static_cast<uint32_t>(*noted)) : Reference();
}

Draft draftScopeRead(Walker& walker, const BodyValue& read, Reference environment)
{
    Draft draft(Opcode::GetEnvironmentField, walker.originOf(read));
    draft.setEnvironmentOperand(environment);
    draft.setLayout(read.layout());
    draft.setField(read.field());
    return draft;
}

void hoistModuleLinks(Walker& walker, BlockID inputHead)
{
    const Program& program = walker.program();
    const BodyIndex& index = walker.currentBodyIndex();
    LoopID loop = index.loopOf(inputHead);
    if (program.moduleInstances.isEmpty() || !keepsNotesOfStableBindings(walker) || index.loop(loop).parent || !index.opcodes().contains(Opcode::GetEnvironmentField))
        return;
    uint32_t firstValueOfLoop = index.block(inputHead).firstParameter().index();
    for (BlockID member : index.membersOf(loop)) {
        for (BodyValue operation : index.block(member).values()) {
            if (walker.isUnreachable())
                return;
            if (!isLinkRead(program, index, operation.id()) || loopInvariantScope(walker, index, operation.id(), firstValueOfLoop))
                continue;
            Reference environment = loopInvariantScope(walker, index, operation.environmentOperand(), firstValueOfLoop);
            if (!environment || !environment.isRecord())
                continue;
            Draft read = draftScopeRead(walker, operation, environment);
            walker.chooseConservativeFields(read);
            Reference link = walker.build(WTF::move(read));
            if (!link)
                return;
            noteValueOfStableBinding(walker, environment, { operation.layout(), operation.field() }, link);
        }
    }
}

Reference valueOfStableBinding(Walker& walker, const BodyValue& read)
{
    return valueOfStableBinding(walker, read, read.opcode() == Opcode::GetEnvironmentField ? walker.referenceOf(read.environmentOperand()) : Reference());
}

Reference valueOfStableBinding(Walker& walker, const BodyValue& read, Reference environment)
{
    if (!keepsNotesOfStableBindings(walker))
        return { };
    std::optional<uint64_t> noted;
    bool readsScopeThatUnitMakes = false;
    if (read.opcode() == Opcode::GetModuleVariable) {
        if (variableStaysAsFilled(walker, read.variable()))
            noted = findKnown(walker, KnownNote::ValueKeptByPlace, { }, { }, read.variable().index());
    } else if (read.opcode() == Opcode::GetEnvironmentField) {
        if (environment && scopeSlotStaysAsFilled(walker, { read.layout(), read.field() }))
            noted = findKnown(walker, KnownNote::ValueKeptByPlace, environment, { }, read.field());
        readsScopeThatUnitMakes = walker.unitHasMadeScopes() && noted && environment.isValue() && walker.code().at(environment.id()).opcode == Opcode::NewObject;
    }
    if (!noted)
        return { };
    Reference kept = Reference::fromBits(static_cast<uint32_t>(*noted));
    TypeID type = walker.typeOf(kept);
    if (type && readsScopeThatUnitMakes && !walker.isInLoop())
        return kept;
    TypeID widest = widestTypeOfOperation(walker, read);
    return widest && type && walker.types().isSubset(type, widest) ? kept : Reference();
}

static std::optional<uint32_t> findScopeField(const Program& program, LayoutID instance, LayoutID scope)
{
    for (uint32_t field = 0; field < program.at(instance).fields.size(); ++field) {
        if (program.isInstanceConstant({ instance, field }) && program.at(program.at(instance).fields[field].declared.written()).environment == scope)
            return field;
    }
    return std::nullopt;
}

bool capturesOnlyModuleScopes(const Walker& walker, FunctionID id)
{
    const Program& program = walker.program();
    const Function& function = program.at(id);
    if (program.moduleInstances.isEmpty() || function.captures.isEmpty() || function.has(FunctionFlags::TakesCallee) || !program.severalInstancesVariable(function.module))
        return false;
    LayoutID instance = program.findModuleInstance(function.module)->layout;
    for (uint32_t capture = 0; capture < function.captures.size(); ++capture) {
        LayoutID scope = scopeThatCaptureTakes(program, id, capture);
        if (walker.plan().promotedEnvironments().isRemoved(id, capture) || !scope || program.at(scope).module != function.module)
            return false;
        if (scope != instance && !findScopeField(program, instance, scope))
            return false;
    }
    return true;
}

static Reference buildTypeTest(Walker& walker, Reference value, TypeID tested, Origin origin)
{
    Draft test(Opcode::TypeTest, origin);
    test.setValueOperand(value);
    test.setTestedType(tested);
    return walker.build(WTF::move(test));
}

static Reference narrowTestedValue(Walker& walker, Reference value, Reference test, bool isOnTakenEdge, Origin origin)
{
    Draft narrowed(Opcode::Narrowed, origin);
    narrowed.setValueOperand(value);
    narrowed.setTestOperand(test);
    narrowed.setOperandIndex(OperandIndex::TypeTest::value);
    narrowed.setIsOnTakenEdge(isOnTakenEdge);
    return walker.build(WTF::move(narrowed));
}

static bool provesSingleInstance(const Walker& walker, Reference hasSeveral)
{
    TypeID type = hasSeveral ? walker.typeOf(hasSeveral) : TypeID();
    return type && walker.types().isSubset(type, TypeTable::undefined);
}

bool hasKnownSingleInstance(const Walker& walker, ModuleID module)
{
    VariableID variable = walker.program().severalInstancesVariable(module);
    return variable && provesSingleInstance(walker, walker.state().variable(variable));
}

static Reference readModuleInstance(Walker& walker, ModuleID module, Origin origin)
{
    VariableID variable = walker.program().instanceVariable(module);
    if (Reference known = walker.state().variable(variable); known && !(walker.types().at(walker.typeOf(known)).primitives & Primitive::Absent))
        return known;
    Draft read(Opcode::GetModuleVariable, origin);
    read.setVariable(variable);
    walker.chooseConservativeFields(read);
    read.setIsKnownInitialized(true);
    Reference instance = walker.build(WTF::move(read));
    if (instance)
        walker.state().noteVariable(variable, instance);
    return instance;
}

std::optional<SingleInstance> findSingleInstance(Walker& walker, ModuleID module, Origin origin)
{
    origin.isTailCall = false;
    VariableID variable = walker.program().severalInstancesVariable(module);
    Reference hasSeveral = walker.state().variable(variable);
    if (!hasSeveral) {
        Draft read(Opcode::GetModuleVariable, origin);
        read.setVariable(variable);
        walker.chooseConservativeFields(read);
        hasSeveral = walker.build(WTF::move(read));
        if (!hasSeveral || !hasSeveral.isValue())
            return std::nullopt;
        walker.state().noteVariable(variable, hasSeveral);
    }
    if (provesSingleInstance(walker, hasSeveral))
        return SingleInstance { readModuleInstance(walker, module, origin), { }, { } };
    if (walker.isInvariantInSpecializedVersion(hasSeveral)) {
        VariableID instanceVariable = walker.program().instanceVariable(module);
        Reference instance;
        Reference isSingleInstance;
        Reference isMissing;
        {
            Walker::SpecializedPreheaderScope preheaderScope(walker, true);
            isSingleInstance = buildTypeTest(walker, hasSeveral, TypeTable::undefined, origin);
            Draft read(Opcode::GetModuleVariable, origin);
            read.setVariable(instanceVariable);
            walker.chooseConservativeFields(read);
            read.setReturnsRawSlot(true);
            instance = walker.build(WTF::move(read));
            isMissing = instance ? buildTypeTest(walker, instance, TypeArena::fromPrimitives(Primitive::Absent), origin) : Reference();
        }
        auto narrowInPreheader = [&](Reference value, Reference test, bool isOnTakenEdge) {
            Walker::SpecializedPreheaderScope preheaderScope(walker, true);
            return narrowTestedValue(walker, value, test, isOnTakenEdge, origin);
        };
        if (isSingleInstance && isMissing && walker.tryBranchInPreheader(isSingleInstance, true)) {
            walker.recordSpeculation();
            if (Reference proof = narrowInPreheader(hasSeveral, isSingleInstance, true)) {
                walker.state().noteVariable(variable, proof);
                Reference filled = walker.tryBranchInPreheader(isMissing, false) ? narrowInPreheader(instance, isMissing, false) : Reference();
                if (filled)
                    walker.state().noteVariable(instanceVariable, filled);
                return SingleInstance { filled ? filled : readModuleInstance(walker, module, origin), { }, { } };
            }
        }
        if (walker.hasFailed() || walker.isUnreachable())
            return std::nullopt;
    }
    Reference isSingleInstance = buildTypeTest(walker, hasSeveral, TypeTable::undefined, origin);
    if (!isSingleInstance || !isSingleInstance.isValue())
        return std::nullopt;
    SingleInstance tested { { }, hasSeveral, isSingleInstance };
    if (!walker.canRestartIteration())
        return tested;
    BlockID leaves = walker.addBlock(BlockFrequency::Rare);
    BlockID hasOneInstance = walker.addBlock();
    walker.branch(isSingleInstance, hasOneInstance, leaves);
    walker.recordSpeculation();
    walker.continueIn(leaves);
    if (!walker.isUnreachable())
        walker.restartIteration();
    walker.continueIn(hasOneInstance);
    if (walker.isUnreachable() || walker.hasFailed())
        return std::nullopt;
    return SingleInstance { instanceAfterTest(walker, module, tested, origin), { }, { } };
}

Reference instanceAfterTest(Walker& walker, ModuleID module, const SingleInstance& tested, Origin origin)
{
    origin.isTailCall = false;
    if (Reference proof = narrowTestedValue(walker, tested.hasSeveral, tested.isSingleInstance, true, origin))
        walker.state().noteVariable(walker.program().severalInstancesVariable(module), proof);
    return readModuleInstance(walker, module, origin);
}

Vector<Reference, 2> capturedModuleScopes(Walker& walker, FunctionID id, Reference instance, Origin origin)
{
    const Program& program = walker.program();
    const Function& function = program.at(id);
    LayoutID layout = program.findModuleInstance(function.module)->layout;
    origin.isTailCall = false;
    Vector<Reference, 2> scopes;
    for (uint32_t capture = 0; instance && capture < function.captures.size(); ++capture) {
        LayoutID scope = scopeThatCaptureTakes(program, id, capture);
        if (scope == layout) {
            scopes.append(instance);
            continue;
        }
        Draft read(Opcode::GetEnvironmentField, origin);
        read.setEnvironmentOperand(instance);
        read.setLayout(layout);
        read.setField(*findScopeField(program, layout, scope));
        walker.chooseConservativeFields(read);
        scopes.append(walker.build(WTF::move(read)));
    }
    return scopes;
}

Reference buildClosureOverScopes(Walker& walker, FunctionID id, std::span<const Reference> scopes, Origin origin)
{
    if (walker.hasFailed() || scopes.size() != walker.program().at(id).captures.size() || std::ranges::any_of(scopes, [](Reference scope) { return !scope; }))
        return { };
    origin.isTailCall = false;
    Draft closure(Opcode::NewClosure, origin);
    closure.standsForNoValueOfInput = true;
    closure.setFunction(id);
    closure.setVariadicOperands(scopes);
    walker.chooseConservativeFields(closure);
    return walker.build(WTF::move(closure));
}

Reference buildClosureOverInstance(Walker& walker, FunctionID id, Reference instance, Origin origin)
{
    Vector<Reference, 2> scopes = capturedModuleScopes(walker, id, instance, origin);
    return buildClosureOverScopes(walker, id, scopes.span(), origin);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
