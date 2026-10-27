/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTResolveAccesses.h"

#if ENABLE(AOT)

#include "AOTBindGlobalNames.h"
#include "AOTBlockFrequencies.h"
#include "AOTBuildContext.h"
#include "AOTBuiltinCalls.h"
#include "AOTBuiltinMembers.h"
#include "AOTBuiltinTable.h"
#include "AOTBuiltinUses.h"
#include "AOTCalleeSet.h"
#include "AOTCodeSizeEstimate.h"
#include "AOTEdgeResults.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsOfAccesses.h"
#include "AOTEffectsOfRules.h"
#include "AOTEscapeAnalysis.h"
#include "AOTFlowApply.h"
#include "AOTFlowResults.h"
#include "AOTFrameAllocationPointers.h"
#include "AOTFunctionAnalyses.h"
#include "AOTHeat.h"
#include "AOTKnownClosures.h"
#include "AOTMemoryLocation.h"
#include "AOTNarrowFieldTypes.h"
#include "AOTObservableOperations.h"
#include "AOTProgramPasses.h"
#include "AOTPropertyAccessCache.h"
#include "AOTPropertyAccessMembers.h"
#include "AOTRegionCloner.h"
#include "AOTResultRules.h"
#include "AOTSlotPlan.h"
#include "AOTSort.h"
#include "AOTSplitUnions.h"
#include "AOTStringArena.h"
#include "AOTThreshold.h"
#include "AOTTypedArrayKinds.h"
#include "AOTTypePropagation.h"
#include "AOTTypeRelations.h"
#include "AOTTypeTestSite.h"
#include "AOTValueIdentity.h"
#include "AOTValueSubstitution.h"
#include "ProxyObject.h"
#include <algorithm>
#include <array>
#include <ranges>
#include <tuple>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet storesListedAtAllocations { Opcode::PropDefine, Opcode::PropSet };

static_assert(PayloadWord::mode(Opcode::ElemSet) && PayloadWord::mode(Opcode::PutArrayElement) && !PayloadWord::mode(Opcode::ElemDefine));

#define FOR_EACH_AOT_STAMPED_SLOT_OPERATION(v) \
    v(GetSlot) \
    v(GetSlotOrUndefined) \
    v(GetSlotGetter) \
    v(GetSlotSetter) \
    v(GetDeferredClosureSlot) \
    v(GetDeferredClosureSlotOrUndefined) \
    v(IsSlotFilled) \
    v(PutSlot) \
    v(PutSlotOrAdd)

#define AOT_CHECK_SLOT_OPERAND_ORDER(operation) \
    static_assert(OperandIndex::operation::object == 0); \
    static_assert(OperandIndex::operation::guard == 1);
FOR_EACH_AOT_STAMPED_SLOT_OPERATION(AOT_CHECK_SLOT_OPERAND_ORDER)
#undef AOT_CHECK_SLOT_OPERAND_ORDER

static_assert(OperandIndex::PutSlot::value == OperandIndex::PutSlotOrAdd::value);

#define AOT_LIST_SLOT_OPERATION(operation) Opcode::operation,
static_assert(Value::opcodesWithFieldAccess == OpcodeSet { FOR_EACH_AOT_STAMPED_SLOT_OPERATION(AOT_LIST_SLOT_OPERATION) });
#undef AOT_LIST_SLOT_OPERATION

#undef FOR_EACH_AOT_STAMPED_SLOT_OPERATION

static bool canMarkIntegralKey(const Value& access, const ElementAccessPlan& plan)
{
    return plan.keyIsIntegral && !plan.testsKeyIsNumber && FlagMask::keyIsIntegral(access.opcode) && !access.keyIsIntegral();
}

static std::optional<PropertyAccessQuery> namedAccessQueryOf(const FunctionPassContext& context, const Value& access)
{
    std::optional<PropertyAccessQuery> query = propertyAccessQueryOf(context.program, context.function, access);
    ASSERT(!query || accessesToResolve.contains(access.opcode));
    if (query)
        query->maxMembers = context.limit(LimitID::MaxAccessPlanMembers);
    return query;
}

std::optional<SiteContent> contentOfNamedAccess(const FunctionPassContext& context, const Value& access, uint8_t planner)
{
    const Program& program = context.program;
    const Function& function = context.function;
    const FlowResults& flow = context.inputs.flow;
    Opcode opcode = access.opcode;
    if (!isNamedAccess(opcode) || (FlagMask::isFreeName(opcode) && access.isFreeName()))
        return std::nullopt;

    SiteContent content(static_cast<uint32_t>(planner) << 24 | static_cast<uint32_t>(opcode) << 8 | access.flags);
    content.add(&flow);
    PropertyKeyID key = keyOfNamedAccess(program, access);
    content.add(key);
    content.add(function.module.index());
    if (familyOf(opcode) == OperationFamily::PrivateAccess)
        content.add(access.layout().index());
    if (std::optional<uint8_t> stored = storedOf(opcode))
        content.add(function.at(access.operand(*stored)).type);
    ValueID receiver = access.operand(*receiverOf(opcode));
    content.addTypesOf(function, receiver);
    if (PayloadWord::receivers(opcode))
        content.add(access.receivers());

    // What a plan takes from the place in the source.
    AccessSiteID site = access.accessSite();
    FunctionID writtenIn = flow.hasResults() && site ? flow.at(site).function : FunctionID();
    bool isWrittenInModuleBody = writtenIn && program.at(writtenIn).isModuleBody();
    content.add(isWrittenInModuleBody ? program.at(writtenIn).module.index() : UINT32_MAX);
    content.add(static_cast<uint32_t>(isAllocationListing(program, function, receiver, key))
        | static_cast<uint32_t>(isUnfinishedAllocationLeavingOut(program, function, receiver, key)) << 1
        | static_cast<uint32_t>(flow.seesOnlyFinishedObjects(site)) << 2
        | static_cast<uint32_t>(!!site) << 3
        | static_cast<uint32_t>(!!writtenIn) << 4);
    if (!content.holdsOnlyIDsOfProgram())
        return std::nullopt;
    return content;
}

std::optional<ElementAccessSite> planElementAccessSite(FunctionPassContext& context, const ValueSite& site)
{
    std::optional<ElementAccessPlan> plan = planElementAccessOf(context.program, context.function, context.function.at(site.value));
    if (!plan)
        return std::nullopt;
    ASSERT(accessesToResolve.contains(context.function.at(site.value).opcode));
    return ElementAccessSite { site, WTF::move(*plan) };
}

// Whoever asks what kind of value it is tells one closure of a function from another no more than a call does.
static bool asksForKindOfValue(Opcode opcode)
{
    Proof proof = provesOf(opcode);
    return proof == Proof::TestedType || proof == Proof::Truthiness;
}

// What the answer proves about the value on an edge is the same value.
static void appendValuesNarrowedBy(const Function& function, const UseLists& uses, ValueID test, ValueID subject, Vector<ValueID, 4>& sameValues)
{
    for (const Use& use : uses.usesOf(test)) {
        if (use.kind != Use::Kind::TerminatorOperand)
            continue;
        const Terminator& terminator = function.at(use.block).terminator;
        for (uint32_t targetIndex = 0; targetIndex < terminator.targets.size(); ++targetIndex) {
            uint32_t firstResult = firstEdgeResultParameter(function, use.block, targetIndex);
            uint32_t numberOfResults = numberOfEdgeResults(function, use.block, targetIndex);
            for (uint32_t resultIndex = 0; resultIndex < numberOfResults; ++resultIndex) {
                if (operandNarrowedOnEdge(function, { use.block, targetIndex, resultIndex }) != subject)
                    continue;
                ValueID narrowed = function.at(terminator.targets[targetIndex].block).parameters[firstResult + resultIndex];
                if (!sameValues.contains(narrowed))
                    sameValues.append(narrowed);
            }
        }
    }
}

bool isOnlyCalled(FunctionAnalyses& analyses, ValueID read)
{
    const Function& function = analyses.function();
    const UseLists& uses = analyses.useLists();

    bool isCalled = false;
    Vector<ValueID, 4> worklist { read };
    while (!worklist.isEmpty()) {
        for (const Use& use : uses.usesOf(worklist.takeLast())) {
            if (use.kind != Use::Kind::Operand)
                return false;

            Opcode user = function.at(use.user).opcode;
            if (isCast(user) && !use.operandIndex) {
                worklist.append(use.user);
                continue;
            }
            if ((user != Opcode::Call || use.operandIndex != OperandIndex::Call::callee) && (user != Opcode::CallList || use.operandIndex != OperandIndex::CallList::callee))
                return false;
            isCalled = true;
        }
    }
    return isCalled;
}

// A call whose callee is known by its type no longer names the value that it called, so what is left of `const method = object.method; if (method !== undefined) method.call(object)` may be the test alone.
static bool isNobodysObject(FunctionAnalyses& analyses, ValueID read)
{
    const Function& function = analyses.function();
    const UseLists& uses = analyses.useLists();

    bool isCalledOrAsked = false;
    Vector<ValueID, 4> sameValues { read };
    for (size_t position = 0; position < sameValues.size(); ++position) {
        ValueID value = sameValues[position];
        for (const Use& use : uses.usesOf(value)) {
            if (use.kind != Use::Kind::Operand)
                return false;

            Opcode user = function.at(use.user).opcode;
            if (isCast(user) && !use.operandIndex) {
                sameValues.append(use.user);
                continue;
            }
            if (asksForKindOfValue(user))
                appendValuesNarrowedBy(function, uses, use.user, value, sameValues);
            else if ((user != Opcode::Call || use.operandIndex != OperandIndex::Call::callee) && (user != Opcode::CallList || use.operandIndex != OperandIndex::CallList::callee))
                return false;
            isCalledOrAsked = true;
        }
    }
    return isCalledOrAsked;
}

AccessOperands operandsOfNamedAccess(const Value& access)
{
    std::optional<uint8_t> stored = storedOf(access.opcode);
    return { access.operand(*receiverOf(access.opcode)), stored ? access.operand(*stored) : ValueID(), { } };
}

SlotStamp createSlotStamp(FunctionPassContext& context, const Value& access, const PropertyAccessPlan& plan)
{
    return {
        keyOfNamedAccess(context.program, access),
        context.types.completeLayoutSet(plan.receivers.span()),
        plan.level,
        PayloadWord::accessSite(access.opcode) ? access.accessSite() : AccessSiteID(),
    };
}

// effectsAfterRewrite() promises the effects of these getters: a call that lists nobody is unknown code to whoever reads the body afterwards.
static CalleeSetID calleesOfGetterInSlot(FunctionPassContext& context, const PropertyAccessPlan& plan, PropertyKeyID key)
{
    std::optional<Vector<FunctionID, 2>> getters = gettersInSlots(context.program, plan.receivers.span(), key);
    if (!getters)
        return { };
    return context.types.intern(CalleeSet::complete(Vector<FunctionID>(getters->span()), { }));
}

Effects effectsAfterRewrite(FunctionPassContext& context, const PropertyAccessPlan& plan, const SlotStamp& stamp)
{
    const Program& program = context.program;
    switch (plan.form) {
    case AccessForm::Slot:
        return effectsOfAccessForm(program, plan.operation, stamp.key, stamp.receivers);
    case AccessForm::Constant:
        return Effects::none();
    case AccessForm::Builtin: {
        Effects effects = declaredBoundOf(Opcode::Builtin);
        if (tryMakeInitializeBuiltin(program.builtins(), context.inputs.flow, plan.member.builtin))
            effects.merge(declaredBoundOf(Opcode::InitializeBuiltin));
        return effects;
    }
    case AccessForm::Method:
        return declaredBoundOf(Opcode::NewClosure);
    case AccessForm::Variable: {
        Value access(plan.operation);
        access.setVariable(plan.member.variable);
        Effects effects = declaredEffectsOf(program, access);
        if (plan.operation == Opcode::GetModuleVariable)
            effects.merge(declaredBoundOf(Opcode::CheckInitialized));
        return effects;
    }
    case AccessForm::Namespace:
        return declaredBoundOf(Opcode::ModuleNamespace);
    case AccessForm::GetterInSlot: {
        std::optional<Vector<FunctionID, 2>> getters = gettersInSlots(program, plan.receivers.span(), stamp.key);
        if (!getters)
            return Effects::unknownCode();
        Effects effects = effectsOfAccessForm(program, plan.operation, stamp.key, stamp.receivers);
        for (FunctionID getter : *getters)
            effects.merge(effectsOfCalling({ program, context.inputs.effects }, getter).normal);
        return effects;
    }
    case AccessForm::SetterInSlot:
        return Effects::unknownCode();
    case AccessForm::ByName:
    case AccessForm::Call:
    case AccessForm::Split:
        return Effects();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Effects effectsAfterRewrite(const ElementAccessPlan& plan, const Type& key)
{
    if (!plan.isResolved() || plan.form == ElementAccessForm::NamedKey || plan.form == ElementAccessForm::ArgumentsElement)
        return Effects();

    bool isInsideLength = plan.testsInBounds || plan.isInBounds;
    if (plan.form == ElementAccessForm::Array)
        isInsideLength |= plan.testsPresence || plan.isPresent;

    // The plan proves that the key is a number. Only the key's type can prove that it is an array index.
    Effects effects = declaredBoundOf(plan.operation);
    refineElementEffects(plan.operation, key, isInsideLength ? ElementPosition::InsideLength : ElementPosition::Anywhere, effects);
    return effects;
}

#if ASSERT_ENABLED
static bool isInsideStoredType(const Program& program, const Type& stored, std::span<const LayoutID> receivers, PropertyKeyID key)
{
    bool slotsHoldUndefinedAndNull = std::ranges::all_of(receivers, [&](LayoutID receiver) {
        std::optional<FieldReference> field = findField(program, receiver, key, BaseChain::Proven);
        return field && slotHoldsUndefinedAndNull(program, *field);
    });
    // Field by field: the type of a brand lies outside `anything`, which storedTypeOf() starts from.
    Type checked = slotsHoldUndefinedAndNull ? stored.withoutNullish() : stored;
    bool hasField = false;
    for (LayoutID receiver : receivers) {
        std::optional<FieldReference> field = findField(program, receiver, key, BaseChain::Proven);
        if (!field)
            continue;
        hasField = true;
        if (!checked.isSubsetOf(program.at(slotTypeOf(program, fieldAt(program, *field))), program))
            return false;
    }
    return hasField;
}
#endif

static ReplacedEffects effectsReplacedAt(FunctionPassContext& context, const Value& value)
{
    ReplacedEffects replaced;
    if (context.build.options().verifiesProgram)
        replaced.add({ context.program, context.inputs.effects }, context.function, value);
    return replaced;
}

static void finishRewrittenAccess(FunctionPassContext& context, const ValueSite& site, const ReplacedEffects& replaced)
{
    Value& value = context.function.edit(site.value);
    value.type = resultTypeOf(context.program, context.function, value, context.types, &context.typingMemo);
    value.setExpected(expectedTypeOf(context.program, context.function, value, context.types, &context.typingMemo));
    checkRewrite(context, site, replaced);
}

void AccessRewriter::rewrite(const NamedAccessSite& site, const AccessOperands& operands)
{
    const Program& program = context.program;
    Function& function = context.function;
    const PropertyAccessPlan& plan = site.plan;
    ValueID id = site.site.value;

    Value* value = &function.edit(id);
    ReplacedEffects replaced = effectsReplacedAt(context, *value);
    // An insertion may move the values.
    auto insertBeforeAccess = [&](Value&& inserted) {
        inserted.setOrigin(value->origin());
        ValueID result = insert(WTF::move(inserted));
        value = &function.edit(id);
        return result;
    };

    switch (plan.form) {
    case AccessForm::Slot: {
        RELEASE_ASSERT(Value::opcodesWithFieldAccess.contains(plan.operation));
        SlotStamp stamp = createSlotStamp(context, *value, plan);
        ASSERT(program.typeOf(function, operands.receiver).isSubsetOf(Type::object(), program));
        ASSERT(operands.guard || program.typeOf(function, operands.receiver).isNever() || program.typeOf(function, operands.receiver).hasOnlyLayouts());
        ASSERT(!plan.stores() || isInsideStoredType(program, program.typeOf(function, operands.stored), plan.receivers.span(), stamp.key));

        value->become(plan.operation);
        value->setOperand(OperandIndex::GetSlot::object, operands.receiver);
        value->setOperand(OperandIndex::GetSlot::guard, operands.guard);
        if (plan.stores())
            value->setOperand(OperandIndex::PutSlot::value, operands.stored);
        setSlotStamp(*value, stamp);
        if (plan.operation == Opcode::GetSlot || plan.operation == Opcode::GetDeferredClosureSlot)
            value->setHoldsDeclaredType(plan.testsDeclaredType && (plan.requiredProofs.contains(Proof::SlotFilled) || plan.requiredProofs.contains(Proof::BirthFieldsFilled)));
        break;
    }
    case AccessForm::GetterInSlot:
    case AccessForm::SetterInSlot: {
        bool callsSetter = plan.form == AccessForm::SetterInSlot;
        RELEASE_ASSERT(callsSetter ? plan.operation == Opcode::GetSlotSetter && value->opcode == Opcode::PropSet : plan.operation == Opcode::GetSlotGetter);
        SlotStamp stamp = createSlotStamp(context, *value, plan);

        Value read = IRBuilder::make(plan.operation, { operands.receiver, operands.guard });
        setSlotStamp(read, { stamp.key, stamp.receivers, stamp.level, { } });
        if (callsSetter)
            read.setIsInStrictCode(value->mode() == ECMAMode::Strict);
        ValueID accessor = insertBeforeAccess(WTF::move(read));

        value->become(Opcode::Call);
        value->setCalleeOperand(accessor);
        value->setThisValueOperand(operands.receiver);
        // The setter is whatever the slot holds: the call tests it for the functions that are expected of it, like any call of a value.
        if (callsSetter)
            value->setArgumentsOperands(std::array { operands.stored });
        else
            value->setCallees(calleesOfGetterInSlot(context, plan, stamp.key));
        value->setAccessSite(stamp.accessSite);
        break;
    }
    case AccessForm::Constant:
        RELEASE_ASSERT(plan.operation == Opcode::Boolean || plan.operation == Opcode::Undefined);
        value->become(plan.operation);
        if (plan.operation == Opcode::Boolean)
            value->setBoolean(plan.booleanConstant);
        break;
    case AccessForm::Builtin:
        if (std::optional<Value> initialization = tryMakeInitializeBuiltin(program.builtins(), context.inputs.flow, plan.member.builtin))
            insertBeforeAccess(WTF::move(*initialization));
        value->become(Opcode::Builtin);
        value->setBuiltin(plan.member.builtin);
        break;
    case AccessForm::Method: {
        FunctionID method = plan.member.method;
        const Vector<DeclaredType>& captures = program.at(method).captures;
        RELEASE_ASSERT(captures.size() == (plan.capturesReceiver ? 1u : 0u));

        ValueID capturedReceiver;
        if (plan.capturesReceiver) {
            AnalysisContext analysisContext { context.build, program, &context.inputs.flow, &context.inputs.effects, context.id };
            capturedReceiver = convertTo(ValueInsertion { analysisContext, function, context.types, value->origin(), insert }, operands.receiver, captures[0]);
            value = &function.edit(id);
        }
        value->become(Opcode::NewClosure);
        value->setFunction(method);
        // A closure that is only called is nobody's object. Any other use may compare it with what another read of the member gave.
        value->setIsHeldByClosureTable(plan.member.kind == MemberKind::Method && (!site.query.isOnlyCalled || program.at(method).has(FunctionFlags::TakesCallee)));
        if (capturedReceiver)
            value->appendVariadicOperand(capturedReceiver);
        break;
    }
    case AccessForm::Variable: {
        VariableID variableID = plan.member.variable;
        const Variable& variable = program.at(variableID);
        RELEASE_ASSERT(plan.operation == Opcode::PutModuleVariable || plan.operation == Opcode::GetModuleVariable);
        // A variable whose type flow has not narrowed keeps the mark of its dead zone in that type, though every read may be proven to come after a store. Whoever uses the read takes a value of the language.
        bool canObserveDeadZone = plan.operation != Opcode::PutModuleVariable && variable.initialization == VariableInitialization::Uninitialized
            && (variable.flow.initialization == InitializationState::Unknown || (!variable.owner && (program.at(variable.declared.written()).primitives & Primitive::Absent)));
        if (!canObserveDeadZone) {
            ASSERT(plan.operation != Opcode::PutModuleVariable || variable.declared.isExpected() || program.typeOf(function, operands.stored).isSubsetOf(program.at(variable.declared.written()), program));
            // In a frame of another function the program reads here, and what the variable holds is known from here on.
            ValueID entered;
            if (plan.operation == Opcode::GetModuleVariable && value->inlineFrame && holdsDeclaredTypeOnlyBehindEntry(variable.flow, liesInCopyOfPredictedCallee(function, value->inlineFrame))) {
                Value fact = IRBuilder::make(Opcode::Entered);
                fact.setFunction(function.inlineFrames[value->inlineFrame].callee);
                entered = insertBeforeAccess(WTF::move(fact));
            }
            value->become(plan.operation);
            value->setVariable(variableID);
            if (plan.operation == Opcode::PutModuleVariable)
                value->setOperand(OperandIndex::PutModuleVariable::value, operands.stored);
            else
                value->setFactOperand(entered);
            break;
        }

        RELEASE_ASSERT(!variable.owner);
        const PropertyKey& exportKey = program.at(site.query.key);
        RELEASE_ASSERT(exportKey.kind == PropertyKeyKind::Text);
        Value read = IRBuilder::make(Opcode::GetModuleVariable);
        read.setVariable(variableID);
        ValueID unchecked = insertBeforeAccess(WTF::move(read));

        value->become(Opcode::CheckInitialized);
        value->setOperand(OperandIndex::CheckInitialized::value, unchecked);
        // The ReferenceError of a namespace object names the export that was read, not the binding the export resolves to.
        value->setName(exportKey.text);
        break;
    }
    case AccessForm::Namespace:
        value->become(Opcode::ModuleNamespace);
        value->setModule(plan.member.module);
        value->setPhase(ImportPhase::Evaluation);
        break;
    case AccessForm::ByName:
    case AccessForm::Call:
    case AccessForm::Split:
        RELEASE_ASSERT_NOT_REACHED();
    }
    finishRewrittenAccess(context, site.site, replaced);
}

void AccessRewriter::rewrite(const ElementAccessSite& site, const AccessOperands& operands)
{
    const ElementAccessPlan& plan = site.plan;
    Value& value = context.function.edit(site.site.value);
    switch (plan.form) {
    case ElementAccessForm::NamedKey:
        RELEASE_ASSERT(plan.constantKey && plan.operation == namedOperationFor(value.opcode));
        becomeNamedAccess(value, internKey(context.types, plan.constantKey));
        finishRewrittenAccess(context, site.site, { });
        return;
    case ElementAccessForm::Array:
        RELEASE_ASSERT(operands.guard || plan.needsNoTest());
        break;
    case ElementAccessForm::TypedArray:
        RELEASE_ASSERT(isTypedView(plan.typedArrayType));
        RELEASE_ASSERT(operands.guard || !plan.isInsideView);
        break;
    case ElementAccessForm::ArgumentsElement:
        // The read stays by key. Only an arguments object that is never built has another form for it.
        if (canMarkIntegralKey(value, plan))
            value.setKeyIsIntegral(true);
        return;
    case ElementAccessForm::ByKey:
    case ElementAccessForm::String:
    case ElementAccessForm::Split:
        value.setKeyIsIntegral(true);
        return;
    }
    RELEASE_ASSERT(plan.form == ElementAccessForm::Array || plan.form == ElementAccessForm::TypedArray);
    RELEASE_ASSERT(!plan.checksNullish);

    ReplacedEffects replaced = effectsReplacedAt(context, value);
    ValueID position = value.propertyOperand();
    AccessSiteID accessSite = value.accessSite();
    bool storageIsUndecided = Value::opcodesWithStorageIsUndecided.contains(value.opcode) && value.storageIsUndecided();
    // A definition throws when it fails, as a strict store does.
    ECMAMode mode = PayloadWord::mode(value.opcode) ? value.mode() : ECMAMode::Strict;

    value.become(plan.operation);
    value.setPositionOperand(position);
    value.setKeyIsIntegral(plan.keyIsIntegral);
    value.setLevel(plan.level);
    value.setAccessSite(accessSite);
    if (OperandPosition::value(plan.operation))
        value.setValueOperand(operands.stored);
    if (plan.form == ElementAccessForm::Array) {
        value.setArrayOperand(operands.receiver);
        value.setGuardOperand(operands.guard);
        value.setArrayStorage(plan.arrayStorage);
        if (Value::opcodesWithIsInBounds.contains(plan.operation))
            value.setIsInBounds(plan.isInBounds);
        if (Value::opcodesWithIsPresent.contains(plan.operation))
            value.setIsPresent(plan.isPresent);
        if (Value::opcodesWithReadsHoleAsNaN.contains(plan.operation))
            value.setReadsHoleAsNaN(plan.readsHoleAsNaN);
        if (Value::opcodesWithStorageIsUndecided.contains(plan.operation))
            value.setStorageIsUndecided(storageIsUndecided);
        if (PayloadWord::mode(plan.operation))
            value.setMode(mode);
    } else {
        value.setViewOperand(operands.receiver);
        if (plan.isInsideView)
            value.setGuardOperand(operands.guard);
        value.setTypedArrayType(plan.typedArrayType);
        value.setIsInsideView(plan.isInsideView);
    }
    finishRewrittenAccess(context, site.site, replaced);
}

// `{ __proto__: null, first: f(), second() { } }`, and `const made = { }; made.first = f(); made.second = g();`.
static bool listFirstStoresAtAllocation(FunctionPassContext& context, IRBuilder& builder, BlockID block, uint32_t allocationIndex)
{
    const Program& program = context.program;
    Function& function = context.function;
    const BlockValues& values = function.at(block).values;
    ValueID allocationID = values[allocationIndex];
    const Layout& layout = program.at(function.at(allocationID).layout());

    Vector<uint32_t, 16> fields(function.at(allocationID).fields());
    Vector<ValueID, 16> listedValues(function.at(allocationID).variadicOperands());
    Vector<PropertyKeyID, 16> writtenNames;
    Vector<ValueID, 2> names { allocationID };
    // What stands between the allocation and the last store that is listed: what does not name the object comes in front of the allocation.
    Vector<ValueID, 32> reordered;
    Vector<ValueID, 4> movedBehindAllocation;
    Vector<ValueID, 32> passed;
    Vector<ValueID, 4> waiting;
    Vector<std::pair<size_t, Value>, 4> fillers;
    size_t numberOfFieldsToList = fields.size();
    size_t numberOfFillersToList = 0;
    bool hasOtherPrototype = !!function.at(allocationID).operand(OperandIndex::NewObject::prototype);
    std::optional<uint32_t> lastListedStore;

    static_assert(OperandIndex::PropDefine::object == OperandIndex::PropSet::object && OperandIndex::PropDefine::object == OperandIndex::SetLiteralPrototype::object && OperandIndex::PropDefine::object == OperandIndex::FinishObject::object);
    for (uint32_t index = allocationIndex + 1; index < values.size(); ++index) {
        const Value& user = function.at(values[index]);
        unsigned numberOfUses = 0;
        bool isReceiver = false;
        uint32_t operandIndex = 0;
        for (ValueID operand : user.operands()) {
            if (names.contains(operand)) {
                ++numberOfUses;
                isReceiver = operandIndex == OperandIndex::PropDefine::object;
            }
            ++operandIndex;
        }
        if (!numberOfUses) {
            passed.append(values[index]);
            continue;
        }
        // Nothing is listed behind the first use that is no store into the object.
        if (!isReceiver || numberOfUses > 1)
            break;
        if (user.opcode == Opcode::FinishObject || user.opcode == Opcode::SetLiteralPrototype) {
            if (user.opcode == Opcode::FinishObject)
                names.append(values[index]);
            else
                hasOtherPrototype = true;
            waiting.append(values[index]);
            continue;
        }
        static_assert(accessesToResolve.contains(Opcode::PropDefine) && accessesToResolve.contains(Opcode::PropSet));
        if (!storesListedAtAllocations.contains(user.opcode))
            break;
        std::optional<uint32_t> field = layout.findField(user.key());
        if (!field || writtenNames.contains(user.key()))
            break;
        writtenNames.append(user.key());
        size_t listedAt = fields.find(*field);
        bool isListed = listedAt != notFound;

        // What a plan wants to know about the state of its receiver is known of this one, or does not matter to an allocation that lists the value.
        ProofSet proofs { Proof::Shape, Proof::OwnInstance, Proof::NoExtraProperties, Proof::NoFieldOutsideSlot, Proof::NotFrozen, Proof::KeyOrder };
        // A store that finds its slot filled looks no further. One that creates the property looks for a setter above the object first, and a definition never does.
        if (isListed || user.opcode == Opcode::PropDefine)
            proofs.add(Proof::SlotFilled);
        if (!hasOtherPrototype)
            proofs.add(Proof::PrototypeLinkUnchanged);
        std::optional<PropertyAccessQuery> query = namedAccessQueryOf(context, user);
        PropertyAccessPlan plan = query ? planPropertyAccess({ program, &context.inputs.flow }, *query) : PropertyAccessPlan();
        bool wouldStoreToSlot = query && plan.form == AccessForm::Slot && plan.stores()
            && plan.receiverTest == ReceiverTest::None && plan.conditions.isEmpty() && (plan.requiredProofs | proofs) == proofs;
        if (!wouldStoreToSlot || !plan.storedValueFits) {
            // With something under the name the property is the object's own: no setter is looked for, and the run time looks at the value.
            if (!isListed) {
                const Field& declaration = fieldAt(program, declarationOf(program, { function.at(allocationID).layout(), *field }));
                std::optional<Value> filler;
                if (wouldStoreToSlot && canBeStoredBehindAllocation(declaration))
                    filler = tryCreateFiller(program, program.at(slotTypeOf(program, declaration)));
                if (!filler)
                    break;
                fillers.append({ fields.size(), WTF::move(*filler) });
                fields.append(*field);
                listedValues.append(ValueID());
            }
            waiting.append(values[index]);
            continue;
        }
        if (isListed)
            listedValues[listedAt] = user.valueOperand();
        else {
            fields.append(*field);
            listedValues.append(user.valueOperand());
        }
        reordered.appendVector(std::exchange(passed, { }));
        movedBehindAllocation.appendVector(std::exchange(waiting, { }));
        lastListedStore = index;
        numberOfFieldsToList = fields.size();
        numberOfFillersToList = fillers.size();
    }
    if (!lastListedStore)
        return false;
    fields.shrink(numberOfFieldsToList);
    listedValues.shrink(numberOfFieldsToList);
    fillers.shrink(numberOfFillersToList);

    BlockValues& editedValues = function.edit(block, BlockEdit::Values).values;
    builder.setOrigin(function.at(allocationID).origin());
    for (auto& [listedAt, filler] : fillers) {
        listedValues[listedAt] = builder.addDetached(WTF::move(filler));
        reordered.append(listedValues[listedAt]);
    }
    reordered.append(allocationID);
    reordered.appendVector(movedBehindAllocation);
    editedValues.removeAt(allocationIndex, *lastListedStore - allocationIndex + 1);
    editedValues.insertVector(allocationIndex, reordered);

    Value& allocation = function.edit(allocationID);
    allocation.setFields(fields.span());
    allocation.setVariadicOperands(listedValues.span());
    context.analyses.didChangeValues();
    return true;
}

static bool listFirstStoresAtAllocations(FunctionPassContext& context, IRBuilder& builder)
{
    Function& function = context.function;
    // Only a store in the block of the allocation is listed, and its receiver is the allocation or what finishes it.
    Vector<BlockID, 8> blocksWithStoreIntoNewObject;
    BitVector isStoredInto;
    auto addAllocationsBehind = [&](auto& self, ValueID receiver) -> bool {
        const Value& value = function.at(receiver);
        if (value.opcode == Opcode::NewObject) {
            isStoredInto.set(receiver.index());
            return true;
        }
        if (value.opcode != Opcode::FinishObject)
            return false;
        for (ValueID operand : value.operands()) {
            if (operand)
                self(self, operand);
        }
        return true;
    };
    for (const ValueSite& site : context.sitesIn(SiteList::Accesses)) {
        const Value& store = function.at(site.value);
        if (!storesListedAtAllocations.contains(store.opcode))
            continue;
        if (addAllocationsBehind(addAllocationsBehind, store.operand(OperandIndex::PropDefine::object)) && (blocksWithStoreIntoNewObject.isEmpty() || blocksWithStoreIntoNewObject.last() != site.block))
            blocksWithStoreIntoNewObject.append(site.block);
    }

    bool didChange = false;
    for (BlockID block : blocksWithStoreIntoNewObject) {
        // An allocation that has moved down is looked at again where it stands now, and finds nothing more.
        for (uint32_t index = 0; index < function.at(block).values.size(); ++index) {
            ValueID id = function.at(block).values[index];
            const Value& value = function.at(id);
            if (value.opcode == Opcode::NewObject && isStoredInto.get(id.index()) && value.fields().size() == value.numberOfVariadicOperands())
                didChange |= listFirstStoresAtAllocation(context, builder, block, index);
        }
    }
    return didChange;
}

static bool needsOnlyFilledSlot(const PropertyAccessPlan& plan)
{
    return plan.receiverTest == ReceiverTest::None && plan.conditions.isEmpty() && plan.requiredProofs == ProofSet { Proof::SlotFilled };
}

// Nothing but the state of the slot is open, and either state has its own read: no lookup by name from the receiver is left.
static bool canGoOnAtPrototype(const PropertyAccessPlan& plan)
{
    return plan.emptySlotMeansNoOwnProperty && needsOnlyFilledSlot(plan);
}

// Whether the block is entered from nowhere but the failure of a test that the slot of this read is filled: it is the read by name that such a test has left behind.
static bool standsBehindFailedTestOfItsSlot(FunctionAnalyses& analyses, const ValueSite& site)
{
    const Function& function = analyses.function();
    const Value& read = function.at(site.value);
    BlockID block = site.block;
    for (unsigned steps = 0; steps < 3; ++steps) {
        std::span<const BlockID> predecessors = analyses.cfg().predecessors(block);
        if (predecessors.size() != 1)
            return false;
        const Terminator& terminator = function.at(predecessors[0]).terminator;
        if (terminator.kind == TerminatorKind::Branch) {
            const Value& test = function.at(terminator.conditionOperand());
            return test.opcode == Opcode::IsSlotFilled && test.key() == read.key() && terminator.targets[0].block != block
                && sameObjectThroughCasts(function, test.objectOperand()) == sameObjectThroughCasts(function, read.objectOperand());
        }
        if (terminator.kind != TerminatorKind::Jump)
            return false;
        block = predecessors[0];
    }
    return false;
}

// Nobody places tests in a generic version or in a block that is marked as a slow path. There the one test that the read of an own accessor needs stands in front of it, and the read by name behind its failure.
static bool mayTestSlotOfGetterWhereItStands(const PropertyAccessPlan& plan)
{
    return plan.form == AccessForm::GetterInSlot && needsOnlyFilledSlot(plan);
}

static bool testsSlotOfGetterWhereItStands(FunctionAnalyses& analyses, const ValueSite& site, const PropertyAccessPlan& plan)
{
    if (!mayTestSlotOfGetterWhereItStands(plan) || standsBehindFailedTestOfItsSlot(analyses, site))
        return false;
    const Function& function = analyses.function();
    switch (function.at(site.block).frequency.predicted()) {
    case BlockFrequency::Normal:
        return function.runsOnlyBehindFailedTest();
    case BlockFrequency::Unlikely:
        return true;
    case BlockFrequency::Rare:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// A store into a slot that is still empty adds the property, so it first asks the objects above for a setter or a read-only property of the name. The prototype of the layout has none.
// An object that was handed another prototype asks by name, and what stands behind the store goes on as it does for every other object.
static bool mayTestPrototypeOfStoreWhereItStands(const PropertyAccessPlan& plan)
{
    return plan.form == AccessForm::Slot && plan.stores() && plan.storedValueFits && plan.receiverTest == ReceiverTest::None
        && plan.requiredProofs.contains(Proof::PrototypeLinkUnchanged) && !plan.requiredProofs.contains(Proof::NotNullish);
}

static bool hasFailedTestOfPrototype(FunctionAnalyses& analyses, ValueID object, BlockID block)
{
    return analyses.findEarlierTest(makeObjectTest(Proof::PrototypeLinkUnchanged, object, { }), block) == EarlierTest::Failed;
}

// The arms cost bytes at every access. Code that seldom runs keeps one test for all the accesses of a stretch.
static bool isWorthArms(FunctionPassContext& context, BlockID block)
{
    return context.function.at(block).frequency.predicted() == BlockFrequency::Normal && !context.function.runsOnlyBehindFailedTest() && !isCold(heatClassOf(context, block)).predicted();
}

// Nobody has seen an object that its constructor is still filling, and a store by name would have to finish it first.
// An object that this function has made is expected to have the prototype it was made with: where nothing stands between, the test costs no code.
static bool leavesTestOfPrototypeToStore(FunctionPassContext& context, const Value& access, const PropertyAccessQuery& query, const PropertyAccessPlan& plan)
{
    return access.opcode == Opcode::PropSet && plan.form == AccessForm::Slot && plan.stores() && plan.storedValueFits && plan.requiredProofs.contains(Proof::PrototypeLinkUnchanged)
        && context.inputs.flow.seesOnlyFinishedObjects(query.site) && context.function.at(context.analyses.identityOf(access.objectOperand())).opcode != Opcode::NewObject;
}

static bool testsPrototypeOfStoreWhereItStands(FunctionPassContext& context, const ValueSite& site, const PropertyAccessQuery& query, const PropertyAccessPlan& plan)
{
    const Value& store = context.function.at(site.value);
    if (!mayTestPrototypeOfStoreWhereItStands(plan) || !leavesTestOfPrototypeToStore(context, store, query, plan) || !isWorthArms(context, site.block))
        return false;
    return !hasFailedTestOfPrototype(context.analyses, store.objectOperand(), site.block);
}

// Nothing above the layout has the name, and flow has seen the read find something: an object that was handed another prototype finds it there.
static bool mayTestPrototypeOfReadWhereItStands(const PropertyAccessPlan& plan)
{
    return plan.form == AccessForm::Constant && plan.operation == Opcode::Undefined && plan.receiverTest == ReceiverTest::None && plan.conditions.isEmpty()
        && plan.requiredProofs.contains(Proof::PrototypeLinkUnchanged) && !plan.requiredProofs.contains(Proof::NotNullish);
}

static bool isProvenToFindMoreThanUndefined(FunctionPassContext& context, ValueID read)
{
    return std::ranges::any_of(context.analyses.useLists().usesOf(read), [&](const Use& use) {
        if (use.kind != Use::Kind::Operand)
            return false;
        const Value& user = context.function.at(use.user);
        return user.opcode == Opcode::AssumeType && user.provenType() != TypeTable::undefined;
    });
}

static bool isProvenToFindFunctions(FunctionPassContext& context, ValueID read, unsigned edgesToFollow = 2)
{
    const Function& function = context.function;
    return std::ranges::any_of(context.analyses.useLists().usesOf(read), [&](const Use& use) {
        if (use.kind == Use::Kind::EdgeArgument && edgesToFollow)
            return isProvenToFindFunctions(context, function.at(targetOf(function, use.edge()).block).parameters[use.operandIndex], edgesToFollow - 1);
        if (use.kind != Use::Kind::Operand)
            return false;
        const Value& user = function.at(use.user);
        if (user.opcode != Opcode::AssumeType)
            return false;
        const Type& found = context.program.at(user.provenType());
        bool holdsFunctions = found.anyFunction || !found.functions.isEmpty() || !found.builtins.isEmpty();
        return holdsFunctions && !(found.primitives & ~Primitive::Undefined) && !found.anyObject && !found.layouts && found.classes.isEmpty();
    });
}

static bool testsPrototypeOfReadWhereItStands(FunctionPassContext& context, const ValueSite& site, const PropertyAccessPlan& plan)
{
    const Value& read = context.function.at(site.value);
    if (read.opcode != Opcode::PropGet || !mayTestPrototypeOfReadWhereItStands(plan) || !isWorthArms(context, site.block))
        return false;
    return !hasFailedTestOfPrototype(context.analyses, read.objectOperand(), site.block) && isProvenToFindMoreThanUndefined(context, site.value);
}

static bool answersWithoutLookup(AccessForm form)
{
    return form == AccessForm::Constant || form == AccessForm::Builtin || form == AccessForm::Method;
}

static bool isRefusedWhereverItStands(const PropertyAccessPlan& plan)
{
    return !answersWithoutLookup(plan.form) && !(plan.isResolved() && plan.needsNoTest()) && !canGoOnAtPrototype(plan) && !mayTestSlotOfGetterWhereItStands(plan) && !mayTestPrototypeOfStoreWhereItStands(plan);
}

static ValueID appendCallOfGetterInSlot(FunctionPassContext& context, IRBuilder& builder, const PropertyAccessPlan& plan, const SlotStamp& stamp, ValueID object, ValueID guard)
{
    const Program& program = context.program;
    Value read = IRBuilder::make(Opcode::GetSlotGetter, { object, guard });
    setSlotStamp(read, { stamp.key, stamp.receivers, stamp.level, { } });
    ValueID getter = builder.append(WTF::move(read));

    std::optional<Vector<FunctionID, 2>> getters = gettersInSlots(program, plan.receivers.span(), stamp.key);
    if (getters && getters->size() == 1 && mayBeCalledDirectly(program, getters->first())) {
        FunctionID callee = getters->first();
        Vector<ValueID, 8> captures = appendCaptures(builder, callee, getter);
        return appendDirectCall(builder, {
            .callee = callee,
            .captures = captures.span(),
            .thisValue = object,
            .calleeValue = program.at(callee).has(FunctionFlags::TakesCallee) ? getter : ValueID(),
            .accessSite = stamp.accessSite,
            .calleeIsPredicted = stamp.level != KnowledgeLevel::Proven,
        });
    }
    Value call = IRBuilder::make(Opcode::Call);
    call.setCalleeOperand(getter);
    call.setThisValueOperand(object);
    call.setCallees(calleesOfGetterInSlot(context, plan, stamp.key));
    call.setAccessSite(stamp.accessSite);
    return builder.append(WTF::move(call));
}

// The receiver may be anything, so whoever resolves the call sees to what the getter takes for `this`.
static ValueID appendCallOfGetterInSlotFor(FunctionPassContext& context, IRBuilder& builder, const PropertyAccessPlan& plan, const SlotStamp& stamp, ValueID object, ValueID guard, ValueID receiver)
{
    Value read = IRBuilder::make(Opcode::GetSlotGetter, { object, guard });
    setSlotStamp(read, { stamp.key, stamp.receivers, stamp.level, { } });
    Value call = IRBuilder::make(Opcode::Call);
    call.setCalleeOperand(builder.append(WTF::move(read)));
    call.setThisValueOperand(receiver);
    call.setCallees(calleesOfGetterInSlot(context, plan, stamp.key));
    return builder.append(WTF::move(call));
}

// The rewrites have moved the sites, and an expansion moves what stands behind it.
static void sortFromLastToFirst(FunctionPassContext& context, Vector<NamedAccessSite>& sites)
{
    const UseLists& useLists = context.analyses.useLists();
    for (NamedAccessSite& site : sites) {
        ValueLocation location = useLists.locationOf(site.site.value);
        RELEASE_ASSERT(location.placement == ValuePlacement::InBlock);
        site.site.block = location.block;
        site.site.index = location.index;
    }
    AOT::sort(sites, std::ranges::greater { }, [](const NamedAccessSite& site) {
        return std::pair { site.site.block, site.site.index };
    });
}

static void expandStoresUnderPrototypesThatMayBeReplaced(FunctionPassContext& context, IRBuilder& builder, Vector<NamedAccessSite>& stores)
{
    if (stores.isEmpty())
        return;

    sortFromLastToFirst(context, stores);
    SiteExpansion expansion(context, builder);
    for (const NamedAccessSite& store : stores) {
        SlotStamp stamp = createSlotStamp(context, context.function.at(store.site.value), store.plan);
        expansion.begin(store.site);
        ValueID object = expansion.original().objectOperand();
        ValueID stored = expansion.original().valueOperand();
        Expected<BlockFrequency> frequency = expansion.frequency();
        // Which prototype an object has is no guess of the compiler's: both arms are laid out alike. The other tests seldom fail.
        BlockID underAnotherPrototype = builder.addBlock(frequency);
        BlockID otherwise;
        Vector<ValueID, 4> guards;
        AccessTests tests = createAccessTests(context, store.plan, { object, stored, stamp });
        for (Value& test : tests.tests) {
            bool asksForPrototype = test.opcode == Opcode::HasLayoutPrototype;
            if (!asksForPrototype && !otherwise)
                otherwise = builder.addBlock(rarerOf(frequency, BlockFrequency::Unlikely));
            BlockID whenFailed = asksForPrototype ? underAnotherPrototype : otherwise;
            ValueID passed = builder.append(WTF::move(test));
            IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(passed, testedOperandIndex, frequency, context.function.at(whenFailed).frequency, { { }, Target { whenFailed, { }, { } } });
            guards.append(branch.provenOnTaken);
            builder.moveToEndOf(branch.taken);
        }
        Value storeBySlot = IRBuilder::make(store.plan.operation, { object, guards.size() == 1 ? guards[0] : builder.appendAllGuards(guards.span()), stored });
        setSlotStamp(storeBySlot, stamp);
        expansion.append(WTF::move(storeBySlot));
        expansion.finishArmWithoutResult();

        for (BlockID byName : { underAnotherPrototype, otherwise }) {
            if (!byName)
                continue;
            builder.moveToEndOf(byName);
            expansion.appendOriginal();
            expansion.finishArmWithoutResult();
        }
        expansion.end();
    }
    context.analyses.didChangeValues();
}

static FunctionID findOnlyGetterCalledAt(FunctionPassContext& context, AccessSiteID site)
{
    if (!site)
        return { };
    FunctionID getter;
    for (const FlowIndirectCall& call : context.inputs.flow.indirectCallsFrom(context.inputs.flow.at(site).function)) {
        if (call.site != site || call.kind != FlowIndirectCallKind::Accessor)
            continue;
        if (getter && getter != call.callee)
            return { };
        getter = call.callee;
    }
    return getter && context.program.at(getter).kind == FunctionKind::Getter && mayBeCalledDirectly(context.program, getter) ? getter : FunctionID();
}

static void expandReadsUnderPrototypesThatMayBeReplaced(FunctionPassContext& context, IRBuilder& builder, Vector<NamedAccessSite>& reads)
{
    if (reads.isEmpty())
        return;

    sortFromLastToFirst(context, reads);
    SiteExpansion expansion(context, builder);
    for (const NamedAccessSite& read : reads) {
        SlotStamp stamp = createSlotStamp(context, context.function.at(read.site.value), read.plan);
        expansion.begin(read.site);
        ValueID object = expansion.original().objectOperand();
        Expected<BlockFrequency> frequency = expansion.frequency();
        Expected<BlockFrequency> unlikelyFrequency = rarerOf(frequency, BlockFrequency::Unlikely);
        // Whoever reads the name expects to find it, and only an object that was handed another prototype does.
        BlockID underAnotherPrototype = builder.addBlock(frequency);
        BlockID otherwise;
        for (Value& test : createAccessTests(context, read.plan, { object, { }, stamp }).tests) {
            bool asksForPrototype = test.opcode == Opcode::HasLayoutPrototype;
            if (!asksForPrototype && !otherwise)
                otherwise = builder.addBlock(unlikelyFrequency);
            BlockID whenFailed = asksForPrototype ? underAnotherPrototype : otherwise;
            ValueID passed = builder.append(WTF::move(test));
            IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(passed, testedOperandIndex, unlikelyFrequency, context.function.at(whenFailed).frequency, { { }, Target { whenFailed, { }, { } } });
            builder.moveToEndOf(branch.taken);
        }
        expansion.finishArm(expansion.append(IRBuilder::make(Opcode::Undefined)));

        builder.moveToEndOf(underAnotherPrototype);
        if (FunctionID getter = findOnlyGetterCalledAt(context, stamp.accessSite)) {
            // What the site found for the last object holds for every object with the same Structure.
            Value lookup = IRBuilder::make(Opcode::GetCachedGetter, { object });
            lookup.setKey(stamp.key);
            ValueID isGetter = builder.appendTypeTest(expansion.append(WTF::move(lookup)), Type::forFunction(getter));
            IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(isGetter, OperandIndex::TypeTest::value, frequency, unlikelyFrequency);
            Vector<ValueID, 8> captures = appendCaptures(builder, getter, branch.provenOnTaken);
            expansion.finishArm(appendDirectCall(builder, {
                .callee = getter,
                .captures = captures.span(),
                .thisValue = object,
                .calleeValue = context.program.at(getter).has(FunctionFlags::TakesCallee) ? branch.provenOnTaken : ValueID(),
                .accessSite = stamp.accessSite,
                .calleeIsPredicted = true,
            }));
            builder.moveToEndOf(branch.notTaken);
        }
        expansion.finishArm(expansion.appendOriginal());
        if (otherwise) {
            builder.moveToEndOf(otherwise);
            expansion.finishArm(expansion.appendOriginal());
        }
        expansion.end();
    }
    context.analyses.didChangeValues();
}

// Every receiver finds one and the same method of its class where it has no property of its own under the name.
static std::optional<PropertyAccessPlan> planMethodAboveEmptySlot(FunctionPassContext& context, const SlotStamp& stamp, const Type& typeOfRead)
{
    std::span<const LayoutID> receivers = context.program.at(stamp.receivers).completeMembers();
    // Whoever makes such an object fills the field: only the code in front of that store finds the slot empty.
    bool isFilledByCreator = std::ranges::any_of(receivers, [&](LayoutID receiver) {
        std::optional<FieldReference> field = findField(context.program, receiver, stamp.key, BaseChain::Proven);
        return field && fieldAt(context.program, *field).flags.contains(FieldFlags::IsFilledByCreator);
    });
    if (isFilledByCreator)
        return std::nullopt;
    MethodsAboveEmptySlot above = findMethodsAboveEmptySlot({ context.program, &context.inputs.flow }, receivers, stamp.key);
    if (!above.areAllReceivers || above.methods.size() != 1 || !Type::forFunction(above.methods[0].method).isSubsetOf(typeOfRead, context.program))
        return std::nullopt;
    PropertyAccessPlan plan;
    plan.form = AccessForm::Method;
    plan.member.method = above.methods[0].method;
    plan.receivers = WTF::move(above.methods[0].layouts);
    plan.requiredProofs = above.methods[0].proofs;
    plan.conditions = WTF::move(above.methods[0].conditions);
    return plan;
}

static void expandReadsOfFieldsThatMayBeEmpty(FunctionPassContext& context, IRBuilder& builder, Vector<NamedAccessSite>& reads)
{
    if (reads.isEmpty())
        return;

    sortFromLastToFirst(context, reads);
    SiteExpansion expansion(context, builder);
    for (const NamedAccessSite& read : reads) {
        SlotStamp stamp = createSlotStamp(context, context.function.at(read.site.value), read.plan);
        bool testsDeclaredType = read.plan.testsDeclaredType;
        expansion.begin(read.site);
        ValueID object = expansion.original().objectOperand();
        std::optional<PropertyAccessPlan> methodAbove = read.plan.form == AccessForm::Slot && !testsDeclaredType ? planMethodAboveEmptySlot(context, stamp, context.program.at(expansion.original().type)) : std::nullopt;
        Value test = makeObjectTest(Proof::SlotFilled, object, stamp);
        test.setHoldsDeclaredType(testsDeclaredType);
        // A field that is listed and holds undefined without storage hides the method.
        test.setCountsFieldsWithoutStorage(!!methodAbove);
        BlockID failedUnasked = builder.beginPrediction();
        ValueID isFilled = builder.append(WTF::move(test));
        Expected<BlockFrequency> frequencyOfFailure = testsDeclaredType ? rarerOf(expansion.frequency(), BlockFrequency::Unlikely) : expansion.frequency();
        IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(isFilled, OperandIndex::IsSlotFilled::object, expansion.frequency(), frequencyOfFailure);
        // Only an empty slot answers with the method. Where no test was asked, the name is looked up.
        BlockID byName = methodAbove ? failedUnasked : BlockID();
        if (!methodAbove)
            builder.endPrediction(failedUnasked, branch, object);

        builder.moveToEndOf(branch.taken);
        if (read.plan.form == AccessForm::GetterInSlot)
            expansion.finishArm(appendCallOfGetterInSlot(context, builder, read.plan, stamp, object, branch.provenOnTaken));
        else {
            Value ownRead = IRBuilder::make(Opcode::GetSlot, { object, branch.provenOnTaken });
            setSlotStamp(ownRead, stamp);
            ownRead.setHoldsDeclaredType(testsDeclaredType);
            expansion.finishArm(expansion.append(WTF::move(ownRead)));
        }

        builder.moveToEndOf(branch.notTaken);
        if (methodAbove) {
            for (Value& testOfMethod : createAccessTests(context, *methodAbove, { object, { }, { stamp.key, stamp.receivers, stamp.level, { } } }).tests) {
                if (!byName)
                    byName = builder.addBlock(rarerOf(frequencyOfFailure, BlockFrequency::Unlikely));
                BlockID next = builder.addBlock(frequencyOfFailure);
                builder.emitBranch(builder.append(WTF::move(testOfMethod)), Target { next, { }, { } }, Target { byName, { }, { } });
                builder.moveToEndOf(next);
            }
            // Whoever compares it with another read of the member finds the same closure.
            Value closure = IRBuilder::make(Opcode::NewClosure);
            closure.setFunction(methodAbove->member.method);
            closure.setIsHeldByClosureTable(true);
            expansion.finishArm(expansion.append(WTF::move(closure)));
            if (byName)
                builder.moveToEndOf(byName);
        }
        if (!methodAbove || byName) {
            // The lookup starts over at the object.
            Value lookup = IRBuilder::make(Opcode::SuperGet, { object, object });
            lookup.setKey(stamp.key);
            expansion.finishArm(expansion.append(WTF::move(lookup)));
        }
        expansion.end();
    }
    context.analyses.didChangeValues();
}

struct ProxyInSight {
    ValueID target;
    ValueID handler;
};

// Nobody can revoke what `new Proxy(target, handler)` makes, so it has that target and that handler for ever.
static std::optional<ProxyInSight> findProxyMadeInSight(const Program& program, const Function& function, ValueID value)
{
    const Value& made = function.at(sameObjectThroughCasts(function, value));
    if (made.opcode != Opcode::CallBuiltin)
        return std::nullopt;
    BuiltinCall call = builtinCallOf(program, made);
    const BuiltinOverload& overload = program.builtins().at(call.overload);
    if (program.builtins().canonicalID(overload.builtin) != Builtins::Proxy || overload.mode != BuiltinCallMode::Construct || call.form != BuiltinCallForm::Whole || call.arguments.size() < 2)
        return std::nullopt;
    return ProxyInSight { call.arguments[0], call.arguments[1] };
}

// The function of Reflect that does what the access does. The trap that the access calls has its name.
static BuiltinID findFunctionOfReflectFor(Opcode access)
{
    if (access == Opcode::PropGet || access == Opcode::ElemGet)
        return Builtins::ReflectGet;
    if (access == Opcode::PropSet || access == Opcode::ElemSet)
        return Builtins::ReflectSet;
    if (access == Opcode::PropHas || access == Opcode::ElemHas)
        return Builtins::ReflectHas;
    return { };
}

// The function that a literal in sight is given under the name. Whoever has the object may have stored another since.
static FunctionID findFunctionGivenAtBirth(const Program& program, const Function& function, ValueID object, PropertyKeyID key)
{
    const Value& allocation = function.at(sameObjectThroughCasts(function, object));
    if (allocation.opcode != Opcode::NewObject)
        return { };
    std::optional<uint32_t> field = program.at(allocation.layout()).findField(key);
    if (!field)
        return { };
    auto fields = allocation.fields();
    auto listed = std::ranges::find(fields, *field);
    if (listed == fields.end())
        return { };
    return program.typeOf(function, allocation.variadicOperands()[listed - fields.begin()]).singleFunction();
}

// The functions that the objects hold under the name, as far as their fields say.
static Vector<FunctionID> findFunctionsInFields(const Program& program, std::span<const LayoutID> layouts, PropertyKeyID key)
{
    Vector<FunctionID> functions;
    for (LayoutID layout : layouts) {
        std::optional<FieldReference> reference = findField(program, layout, key, BaseChain::Proven);
        if (!reference)
            continue;
        const Field& field = fieldAt(program, *reference);
        if (FunctionID deferred = field.flow.location.deferredFunction)
            insertInUniquedSortedVector(functions, deferred);
        for (FunctionID function : program.at(slotTypeOf(program, field)).functions)
            insertInUniquedSortedVector(functions, function);
    }
    return functions;
}

// Goes on where every test passes. A test behind a test of the object's type asks what passed that test. Answers the object as the last such test proved it.
static ValueID emitTestsOfObject(IRBuilder& builder, std::span<Value> tests, ValueID object, BlockID whenFailed, Expected<BlockFrequency> frequency, Expected<BlockFrequency> frequencyOfFailure, ValueID* guardOfFilledSlot = nullptr)
{
    ValueID proven = object;
    for (Value& test : tests) {
        bool provesType = !provesForStretch(provesOf(test.opcode));
        bool provesFilledSlot = provesOf(test.opcode) == Proof::SlotFilled;
        if (test.operand(testedOperandIndex) == object)
            test.setOperand(testedOperandIndex, proven);
        else
            provesType = false;
        IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(builder.append(WTF::move(test)), testedOperandIndex, frequency, frequencyOfFailure);
        builder.moveToEndOf(branch.notTaken);
        builder.emitJump(whenFailed);
        builder.moveToEndOf(branch.taken);
        if (provesType)
            proven = branch.provenOnTaken;
        if (provesFilledSlot && guardOfFilledSlot)
            *guardOfFilledSlot = branch.provenOnTaken;
    }
    return proven;
}

struct TestsOfAbsentTrap {
    // Where all of them pass, an empty slot reads as undefined.
    Vector<Value, 4> tests;
    Value isSlotFilled;
};

static std::optional<TestsOfAbsentTrap> createTestsOfAbsentTrap(FunctionPassContext& context, ValueID handler, PropertyKeyID nameOfTrap)
{
    Value read = IRBuilder::make(Opcode::PropGet, { handler });
    read.setKey(nameOfTrap);
    std::optional<PropertyAccessQuery> query = namedAccessQueryOf(context, read);
    if (!query || !query->receivers.isProven())
        return std::nullopt;
    query->expectsEmptySlot = true;
    PropertyAccessPlan plan = planPropertyAccess({ context.program, &context.inputs.flow }, *query);
    if (!plan.isResolved() || plan.form != AccessForm::Slot || plan.requiredProofs.contains(Proof::SlotFilled))
        return std::nullopt;
    SlotStamp stamp = createSlotStamp(context, read, plan);
    return TestsOfAbsentTrap { createAccessTests(context, plan, { handler, { }, stamp }).tests, makeObjectTest(Proof::SlotFilled, handler, stamp) };
}

enum class ReceiverOfAccess : bool { MayBeProxy, IsProxy };

// As ProxyObject.cpp performProxyGet(), performHasProperty() and performPut(). The trap is read off the handler and called as the program would read and call it, so what is known of the handler and of the target is known here.
static void expandAccessThroughProxy(FunctionPassContext& context, IRBuilder& builder, ProgramPoint point, const std::optional<ProxyInSight>& proxyInSight, const FlowProxiesSeenAtSite* proxiesSeen, ReceiverOfAccess receiver)
{
    const Program& program = context.program;
    Function& function = context.function;
    const Value access = function.at(function.at(point.block).values[point.index]);
    const BuiltinID functionOfReflect = findFunctionOfReflectFor(access.opcode);
    const bool reads = functionOfReflect == Builtins::ReflectGet;
    const bool stores = functionOfReflect == Builtins::ReflectSet;
    const bool isByName = PayloadWord::key(access.opcode).has_value();
    ValueID proxy = access.objectOperand();
    const Expected<BlockFrequency> frequency = function.at(point.block).frequency;
    const Expected<BlockFrequency> unlikely = rarerOf(frequency, BlockFrequency::Unlikely);
    const Expected<BlockFrequency> rare = rarerOf(frequency, BlockFrequency::Rare);
    PropertyKeyID nameOfTrap = program.keyOf(program.builtins().at(functionOfReflect).name);

    IRBuilder::Expansion expansion = builder.beginExpansion(point);
    builder.setOrigin(access.origin());
    auto throwTypeError = [&](ASCIILiteral message) {
        Value error = IRBuilder::make(Opcode::ThrowNewError);
        error.setErrorKind(ErrorKind::TypeError);
        error.setMessage(context.strings.intern(StringView(message)));
        builder.append(WTF::move(error));
        builder.emitUnreachable();
    };
    BlockID done = builder.addBlock(frequency);
    ValueID answer = stores ? ValueID() : builder.addParameter(done, reads ? TypeTable::anything : TypeTable::boolean);
    auto finishWith = [&](ValueID value) {
        if (stores)
            builder.emitJump(done);
        else
            builder.emitJump(done, { value });
    };
    // Where something is not what it is expected to be, the access is made as it is written.
    BlockID asWritten;
    auto blockOfAccessAsWritten = [&](Expected<BlockFrequency> frequencyOfFailure) {
        if (!asWritten)
            asWritten = builder.addBlock(frequencyOfFailure);
        return asWritten;
    };
    auto goOnWhere = [&](ValueID value, TypeID tested, Expected<BlockFrequency> frequencyOfFailure) {
        IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(builder.appendTypeTest(value, tested), 0, frequency, frequencyOfFailure);
        builder.moveToEndOf(branch.notTaken);
        builder.emitJump(blockOfAccessAsWritten(frequencyOfFailure));
        builder.moveToEndOf(branch.taken);
        return branch.provenOnTaken;
    };
    if (receiver == ReceiverOfAccess::MayBeProxy)
        proxy = goOnWhere(proxy, context.types.intern(Type::builtinClass(BuiltinClasses::Proxy)), frequency);
    auto appendFieldRead = [&](ProxyObject::Field field) {
        Value read = IRBuilder::make(Opcode::GetInternalField, { proxy });
        read.setIndex(static_cast<uint32_t>(field));
        return builder.append(WTF::move(read));
    };

    // A key that is a text or a symbol is what the trap is handed. Any other is first made one, which may run code: that is left to the access as it is written.
    ValueID name;
    if (isByName) {
        Value nameOfAccess = IRBuilder::make(Opcode::String);
        nameOfAccess.setString(program.at(access.key()).text);
        name = builder.append(WTF::move(nameOfAccess));
    } else
        name = goOnWhere(access.propertyOperand(), context.types.intern(Type::fromPrimitives(Primitive::AnyString | Primitive::AnySymbol)), unlikely);
    ValueID target;
    BlockID withoutTrap = builder.addBlock(frequency);

    // Every way out of it ends in a jump or throws.
    auto askHandler = [&](ValueID handler, Vector<FunctionID>&& expectedTraps) {
        Expected<BlockFrequency> frequencyOfRead = frequency;
        // A handler that nobody has seen given the trap is first asked whether it is still without it.
        if (expectedTraps.isEmpty()) {
            if (std::optional<TestsOfAbsentTrap> absence = createTestsOfAbsentTrap(context, handler, nameOfTrap)) {
                BlockID mayHaveTrap = builder.addBlock(unlikely);
                absence->isSlotFilled.setOperand(testedOperandIndex, emitTestsOfObject(builder, absence->tests.mutableSpan(), handler, mayHaveTrap, frequency, unlikely));
                IRBuilder::ProvenBranch isFilled = builder.emitBranchWithProof(builder.append(WTF::move(absence->isSlotFilled)), testedOperandIndex, unlikely, frequency);
                builder.moveToEndOf(isFilled.taken);
                builder.emitJump(mayHaveTrap);
                builder.moveToEndOf(isFilled.notTaken);
                builder.emitJump(withoutTrap);
                builder.moveToEndOf(mayHaveTrap);
                frequencyOfRead = unlikely;
            }
        }
        Value readOfTrap = IRBuilder::make(Opcode::PropGet, { handler });
        readOfTrap.setKey(nameOfTrap);
        ValueID trap = builder.append(WTF::move(readOfTrap));
        IRBuilder::ProvenBranch isFunction = builder.emitBranchWithProof(builder.appendTypeTest(trap, TypeTable::function), 0, frequencyOfRead, frequencyOfRead);

        builder.moveToEndOf(isFunction.taken);
        Value call = IRBuilder::make(Opcode::Call);
        call.setCalleeOperand(isFunction.provenOnTaken);
        call.setThisValueOperand(handler);
        if (!expectedTraps.isEmpty())
            call.setCallees(context.types.intern(CalleeSet::open(WTF::move(expectedTraps), { })));
        if (reads)
            call.setArgumentsOperands(std::array { target, name, proxy });
        else if (stores)
            call.setArgumentsOperands(std::array { target, name, access.valueOperand(), proxy });
        else
            call.setArgumentsOperands(std::array { target, name });
        ValueID answerOfTrap = builder.append(WTF::move(call));
        if (reads) {
            IRBuilder::BranchBlocks mayBind = builder.emitBranch(builder.append(Opcode::MayBindAnswersOfGetAndSetTraps, { target }), rare, frequencyOfRead);
            builder.moveToEndOf(mayBind.taken);
            builder.append(Opcode::ValidateAnswerOfGetTrap, { target, name, answerOfTrap });
            finishWith(answerOfTrap);
            builder.moveToEndOf(mayBind.notTaken);
            finishWith(answerOfTrap);
        } else if (stores) {
            // A trap that says no makes a strict store throw.
            BlockID finishes = builder.addBlock(rare);
            IRBuilder::BranchBlocks saysYes = builder.emitBranch(builder.append(Opcode::ToBoolean, { answerOfTrap }), frequencyOfRead, rare);
            builder.moveToEndOf(saysYes.notTaken);
            builder.emitJump(finishes);
            builder.moveToEndOf(saysYes.taken);
            builder.emitBranch(builder.append(Opcode::MayBindAnswersOfGetAndSetTraps, { target }), Target { finishes, { }, { } }, Target { done, { }, { } });
            builder.moveToEndOf(finishes);
            Value finish = IRBuilder::make(Opcode::FinishStoreByTrap, { target, name, access.valueOperand(), answerOfTrap });
            finish.setMode(access.mode());
            builder.append(WTF::move(finish));
            finishWith({ });
        } else {
            IRBuilder::BranchBlocks saysYes = builder.emitBranch(builder.append(Opcode::ToBoolean, { answerOfTrap }), frequencyOfRead, frequencyOfRead);
            builder.moveToEndOf(saysYes.taken);
            finishWith(builder.appendBoolean(true));
            builder.moveToEndOf(saysYes.notTaken);
            IRBuilder::BranchBlocks mayBind = builder.emitBranch(builder.append(Opcode::MayBindAnswersOfHasTraps, { target }), rare, frequencyOfRead);
            builder.moveToEndOf(mayBind.taken);
            builder.append(Opcode::ValidateDenialOfHasTrap, { target, name });
            finishWith(builder.appendBoolean(false));
            builder.moveToEndOf(mayBind.notTaken);
            finishWith(builder.appendBoolean(false));
        }

        builder.moveToEndOf(isFunction.notTaken);
        IRBuilder::BranchBlocks isAbsent = builder.emitBranch(builder.appendTypeTest(trap, TypeTable::nullish), frequencyOfRead, rare);
        builder.moveToEndOf(isAbsent.notTaken);
        throwTypeError(reads ? "'get' property of a Proxy's handler should be callable"_s : stores ? "'set' property of a Proxy's handler should be callable"_s : "'has' property of a Proxy's handler should be callable"_s);
        builder.moveToEndOf(isAbsent.taken);
        builder.emitJump(withoutTrap);
    };

    if (proxyInSight) {
        target = proxyInSight->target;
        Vector<FunctionID> expectedTraps;
        if (FunctionID expected = findFunctionGivenAtBirth(program, function, proxyInSight->handler, nameOfTrap))
            expectedTraps.append(expected);
        askHandler(proxyInSight->handler, WTF::move(expectedTraps));
    } else {
        ValueID heldHandler = appendFieldRead(ProxyObject::Field::Handler);
        ValueID heldTarget = appendFieldRead(ProxyObject::Field::Target);
        const auto& handlers = proxiesSeen->handlers;
        ASSERT(!handlers.isEmpty());
        // Other proxies may arrive than those seen, so what is read out of one is tested. A proxy that has been revoked holds null for both. They are left to the access as it was written.
        target = goOnWhere(heldTarget, proxiesSeen->targets.isEmpty() ? TypeTable::object : context.types.intern(Type::forLayouts(proxiesSeen->targets.span(), context.types)), unlikely);
        {
            // Each kind of handler is asked by itself, so that its trap is known.
            for (LayoutID layout : handlers) {
                IRBuilder::ProvenBranch isOfLayout = builder.emitBranchWithProof(builder.appendTypeTest(heldHandler, context.types.intern(Type::forLayout(layout, context.types))), 0, frequency, handlers.size() == 1 ? unlikely : frequency);
                builder.moveToEndOf(isOfLayout.taken);
                askHandler(isOfLayout.provenOnTaken, findFunctionsInFields(program, singleElementSpan(layout), nameOfTrap));
                builder.moveToEndOf(isOfLayout.notTaken);
            }
            builder.emitJump(blockOfAccessAsWritten(unlikely));
        }
    }

    // Without a trap the target is asked, for the proxy.
    builder.moveToEndOf(withoutTrap);
    if (reads) {
        Value read = IRBuilder::make(Opcode::SuperGet);
        read.setBaseOperand(target);
        read.setReceiverOperand(proxy);
        if (isByName)
            read.setKey(access.key());
        else
            read.setComputedKeyOperand(name);
        finishWith(builder.append(WTF::move(read)));
    } else if (stores) {
        Value store = IRBuilder::make(Opcode::SuperSet);
        store.setMode(access.mode());
        store.setBaseOperand(target);
        store.setReceiverOperand(proxy);
        store.setValueOperand(access.valueOperand());
        if (isByName)
            store.setKey(access.key());
        else
            store.setComputedKeyOperand(name);
        builder.append(WTF::move(store));
        finishWith({ });
    } else if (isByName) {
        Value test = IRBuilder::make(Opcode::PropHas, { target });
        test.setKey(access.key());
        finishWith(builder.append(WTF::move(test)));
    } else
        finishWith(builder.append(Opcode::ElemHas, { target, name }));

    if (asWritten) {
        builder.moveToEndOf(asWritten);
        ValueID written = builder.append(Value(access));
        // It is not asked again whether its receiver is a proxy.
        context.refusedSites.add(RefusalKind::Access, function, written, RefusedSites::stateOf(function, written));
        finishWith(written);
    }

    builder.moveToEndOf(done);
    if (stores)
        builder.endExpansion(expansion, std::span<const ValueID> { });
    else
        builder.endExpansion(expansion, answer);
}

static bool expandAccessesThroughProxies(FunctionPassContext& context, IRBuilder& builder)
{
    const Program& program = context.program;
    Function& function = context.function;
    std::optional<ValueSite> found;
    std::optional<ProxyInSight> proxy;
    ReceiverOfAccess receiver = ReceiverOfAccess::IsProxy;
    // A body is pasted into its callers as it stands in front of the rounds, and there the proxy may be in sight.
    const bool expandsOutOfSight = context.isInRounds && context.inputs.heat.hasWarmCode(program, context.id).predicted();
    const FlowProxiesSeenAtSite* proxiesSeen = nullptr;
    context.forEachSiteIn(SiteList::Accesses, [&](const ValueSite& site) {
        const Value& access = function.at(site.value);
        if (found || !findFunctionOfReflectFor(access.opcode) || (FlagMask::isFreeName(access.opcode) && access.isFreeName()) || (PayloadWord::key(access.opcode) && program.at(access.key()).kind != PropertyKeyKind::Text))
            return;
        if (!PayloadWord::key(access.opcode) && !(program.typeOf(function, access.propertyOperand()).primitives & (Primitive::AnyString | Primitive::AnySymbol)))
            return;
        const Type& typeOfReceiver = program.typeOf(function, access.objectOperand());
        const ClassPart* part = typeOfReceiver.singleBuiltinClass();
        bool isProxyIfObject = part && part->builtinClass == BuiltinClasses::Proxy;
        if (!isProxyIfObject && !typeOfReceiver.anyObject)
            return;
        std::optional<ProxyInSight> inSight = isProxyIfObject ? findProxyMadeInSight(program, function, access.objectOperand()) : std::nullopt;
        const FlowProxiesSeenAtSite* seen = nullptr;
        if (!inSight) {
            // Flow has seen proxies of the program arrive here, and knows the handlers they were made with: only then can a trap be called directly. Elsewhere, and in code that seldom runs, the run time asks in fewer bytes.
            // An access that is written out here for the target of a proxy has no site, so it is not written out again.
            seen = expandsOutOfSight ? context.inputs.flow.findProxiesSeenAt(access.accessSite()) : nullptr;
            if (!seen || seen->handlers.isEmpty())
                return;
            BlockID block = site.point().block;
            if (function.at(block).frequency.predicted() != BlockFrequency::Normal || isCold(heatClassOf(context, block)).predicted())
                return;
        }
        if (context.refusedSites.contains(RefusalKind::Access, site.value, RefusedSites::stateOf(function, site.value)))
            return;
        proxy = inSight;
        proxiesSeen = seen;
        // `let proxy; try { proxy = new Proxy(..); } catch { }` leaves undefined or a proxy.
        receiver = isProxyIfObject && typeOfReceiver.isObject() ? ReceiverOfAccess::IsProxy : ReceiverOfAccess::MayBeProxy;
        found = site;
    });
    if (!found)
        return false;
    expandAccessThroughProxy(context, builder, found->point(), proxy, proxiesSeen, receiver);
    return true;
}

// An access was written out before the call that makes the proxy was inlined.
static bool forwardReadsOutOfProxiesInSight(FunctionPassContext& context)
{
    Function& function = context.function;
    Vector<std::pair<ValueID, ValueID>, 4> found;
    context.forEachSiteIn(SiteList::Accesses, [&](const ValueSite& site) {
        const Value& read = function.at(site.value);
        if (read.opcode != Opcode::GetInternalField)
            return;
        if (std::optional<ProxyInSight> proxy = findProxyMadeInSight(context.program, function, read.objectOperand()))
            found.append({ site.value, read.index() == static_cast<uint32_t>(ProxyObject::Field::Handler) ? proxy->handler : proxy->target });
    });
    for (auto [read, held] : found)
        becomeUpcast(function, read, held);
    return !found.isEmpty();
}

// `Reflect.get(object, "name", object)` is `object.name`.
static bool readByNameWhereReceiverIsTheObject(FunctionPassContext& context)
{
    const Program& program = context.program;
    Function& function = context.function;
    Vector<std::pair<ValueID, PropertyKeyID>, 4> found;
    context.forEachSiteIn(SiteList::Accesses, [&](const ValueSite& site) {
        const Value& read = function.at(site.value);
        if (read.opcode != Opcode::GetByValWithThis || sameObjectThroughCasts(function, read.objectOperand()) != sameObjectThroughCasts(function, read.thisValueOperand()))
            return;
        if (PropertyKeyID key = internKey(context.types, constantNameOf(program, program.sourceFunctionOf(function, read), program.typeOf(function, read.propertyOperand()))))
            found.append({ site.value, key });
    });
    for (auto [id, key] : found) {
        Value& read = function.edit(id);
        ValueID object = read.objectOperand();
        read.become(Opcode::PropGet);
        read.setObjectOperand(object);
        read.setKey(key);
    }
    return !found.isEmpty();
}

// A property of the object's own that lies in its slot is a value, whoever asks for it: the read by name behind the tests finds what they found. A getter that lies there is called for the receiver.
static bool expandReadForAnotherReceiver(FunctionPassContext& context, IRBuilder& builder)
{
    const Program& program = context.program;
    Function& function = context.function;
    std::optional<ValueSite> found;
    Vector<Value, 4> tests;
    ValueID base;
    ValueID receiver;
    PropertyKeyID key;
    std::optional<PropertyAccessPlan> planOfGetter;
    SlotStamp stamp;
    context.forEachSiteIn(SiteList::Accesses, [&](const ValueSite& site) {
        const Value& read = function.at(site.value);
        if (found || !readsForAnotherReceiver.contains(read.opcode))
            return;
        if (read.opcode == Opcode::SuperGet) {
            base = read.baseOperand();
            receiver = read.receiverOperand();
            key = read.baseOperand() == read.receiverOperand() ? PropertyKeyID() : read.key();
        } else {
            base = read.objectOperand();
            receiver = read.thisValueOperand();
            key = internKey(context.types, constantNameOf(program, program.sourceFunctionOf(function, read), program.typeOf(function, read.propertyOperand())));
        }
        if (!key)
            return;
        // What is left where a test fails stands in such a block.
        if (function.at(site.point().block).frequency.predicted() != BlockFrequency::Normal)
            return;
        uint32_t state = RefusedSites::stateOf(function, site.value);
        if (context.refusedSites.contains(RefusalKind::Access, site.value, state))
            return;
        Value readForObject = IRBuilder::make(Opcode::PropGet, { base });
        readForObject.setKey(key);
        std::optional<PropertyAccessQuery> query = namedAccessQueryOf(context, readForObject);
        std::optional<PropertyAccessPlan> plan;
        if (query && query->receivers.isProven())
            plan = planPropertyAccess({ program, &context.inputs.flow }, *query);
        bool findsValue = plan && plan->isResolved() && plan->form == AccessForm::Slot && (plan->needsNoTest() || plan->requiredProofs.contains(Proof::SlotFilled));
        bool findsGetter = plan && plan->isResolved() && plan->form == AccessForm::GetterInSlot && plan->requiredProofs.contains(Proof::SlotFilled);
        if (!findsValue && !findsGetter) {
            context.refusedSites.add(RefusalKind::Access, function, site.value, state);
            return;
        }
        stamp = createSlotStamp(context, readForObject, *plan);
        tests = createAccessTests(context, *plan, { base, { }, stamp }).tests;
        if (findsGetter)
            planOfGetter = WTF::move(plan);
        found = site;
    });
    if (!found)
        return false;

    if (tests.isEmpty()) {
        Value& read = function.edit(found->value);
        read.become(Opcode::PropGet);
        read.setObjectOperand(base);
        read.setKey(key);
        return true;
    }

    ProgramPoint point = found->point();
    const Value read = function.at(found->value);
    Expected<BlockFrequency> frequency = function.at(point.block).frequency;
    Expected<BlockFrequency> unlikely = rarerOf(frequency, BlockFrequency::Unlikely);
    IRBuilder::Expansion expansion = builder.beginExpansion(point);
    builder.setOrigin(read.origin());
    BlockID done = builder.addBlock(frequency);
    ValueID answer = builder.addParameter(done, TypeTable::anything);
    BlockID otherwise = builder.addBlock(unlikely);
    ValueID guardOfFilledSlot;
    ValueID testedBase = emitTestsOfObject(builder, tests.mutableSpan(), base, otherwise, frequency, unlikely, &guardOfFilledSlot);
    if (planOfGetter) {
        builder.emitJump(done, { appendCallOfGetterInSlotFor(context, builder, *planOfGetter, stamp, testedBase, guardOfFilledSlot, receiver) });
    } else {
        Value readForObject = IRBuilder::make(Opcode::PropGet, { testedBase });
        readForObject.setKey(key);
        builder.emitJump(done, { builder.append(WTF::move(readForObject)) });
    }

    builder.moveToEndOf(otherwise);
    builder.emitJump(done, { builder.append(Value(read)) });

    builder.moveToEndOf(done);
    builder.endExpansion(expansion, answer);
    return true;
}

PassOutcome resolveAccesses(FunctionPassContext& context)
{
    const Program& program = context.program;
    Function& function = context.function;
    IRBuilder builder({ context.build, program, &context.inputs.flow, &context.inputs.effects, context.id }, function, context.types, &context.analyses);
    bool didListStores = listFirstStoresAtAllocations(context, builder);
    // An expansion moves what stands behind it, so each looks for its site anew. All are made before a test is placed for one of them.
    bool makesProxies = !context.inputs.flow.usesOf(Builtins::Proxy).isEmpty() || !context.inputs.flow.usesOf(Builtins::ProxyRevocable).isEmpty();
    auto holds = [&](Opcode opcode) {
        return !context.opcodesWhereRunBegins || context.opcodesWhereRunBegins->contains(opcode);
    };
    bool didExpand = holds(Opcode::GetByValWithThis) && readByNameWhereReceiverIsTheObject(context);
    didExpand |= makesProxies && holds(Opcode::GetInternalField) && forwardReadsOutOfProxiesInSight(context);
    while ((makesProxies && expandAccessesThroughProxies(context, builder)) || expandReadForAnotherReceiver(context, builder))
        didExpand = true;
    if (didExpand)
        return PassOutcome::Changed;

    Vector<NamedAccessSite> namedSites;
    Vector<NamedAccessSite> readsOfFieldsThatMayBeEmpty;
    Vector<NamedAccessSite> storesUnderPrototypesThatMayBeReplaced;
    Vector<NamedAccessSite> readsUnderPrototypesThatMayBeReplaced;
    Vector<ElementAccessSite> elementSites;
    PropertyAccessCache* cache = program.propertyAccessCache();
    context.forEachSiteIn(SiteList::Accesses, [&](const ValueSite& site) {
        const Value& access = function.at(site.value);
        // expandReadForAnotherReceiver() and forwardReadsOutOfProxiesInSight() have looked at them.
        if (readsForAnotherReceiver.contains(access.opcode) || access.opcode == Opcode::GetInternalField)
            return;
        uint32_t state = RefusedSites::stateOf(function, site.value);
        if (context.refusedSites.contains(RefusalKind::Access, site.value, state))
            return;
        auto refuse = [&](LimitID limit = LimitID::None) {
            if (limit != LimitID::None)
                context.gaveUp(limit, site.value);
            context.refusedSites.add(RefusalKind::Access, function, site.value, state);
        };
        std::optional<SiteContent> content = cache ? contentOfNamedAccess(context, access, static_cast<uint8_t>(RefusalKind::Access)) : std::nullopt;
        std::optional<uint32_t> limitOfKnownRefusal = content ? cache->refusedContents.find(*content) : std::nullopt;
        if (limitOfKnownRefusal && !cache->checksAnswers) [[likely]] {
            refuse(static_cast<LimitID>(*limitOfKnownRefusal));
            return;
        }
        auto refuseWhereverItStands = [&](LimitID limit) {
            RELEASE_ASSERT(!limitOfKnownRefusal || *limitOfKnownRefusal == static_cast<uint32_t>(limit));
            refuse(limit);
            if (content && !limitOfKnownRefusal)
                cache->refusedContents.add(*content, static_cast<uint32_t>(limit));
        };
        if (std::optional<PropertyAccessQuery> query = namedAccessQueryOf(context, access)) {
            // A plan for such a receiver begins with a test of the receiver, which PlaceTypeTests places.
            if (!query->receivers.isProven()) {
                refuseWhereverItStands(limitOfPropertyAccessPlan(program, *query));
                return;
            }
            PropertyAccessContext accessContext { program, &context.inputs.flow };
            PropertyAccessPlan plan = planPropertyAccess(accessContext, *query);
            bool isRefused = isRefusedWhereverItStands(plan);
            bool isRefusedForItsUses = false;
            if (query->planFollowsFromIsOnlyCalled && access.opcode == Opcode::PropGet) {
                // Who uses what is read is asked, of the lists of all uses in the function, only where the two answers lead to two ends.
                query->isOnlyCalled = true;
                PropertyAccessPlan planOfCalledRead = planPropertyAccess(accessContext, *query);
                isRefused = isRefused && isRefusedWhereverItStands(planOfCalledRead) && planOfCalledRead.limit == plan.limit;
                if (!isRefused) {
                    query->isOnlyCalled = isNobodysObject(context.analyses, site.value);
                    if (query->isOnlyCalled)
                        plan = WTF::move(planOfCalledRead);
                    else
                        isRefusedForItsUses = true;
                }
            }
            if (isRefused) {
                refuseWhereverItStands(plan.limit);
                return;
            }
            RELEASE_ASSERT(!limitOfKnownRefusal);
            if (answersWithoutLookup(plan.form) && isTestedByValidation(function, context.analyses.useLists(), site.value))
                return;
            if (plan.isResolved() && plan.needsNoTest() && plan.form != AccessForm::Call)
                namedSites.append({ site, WTF::move(*query), WTF::move(plan) });
            else if (canGoOnAtPrototype(plan) || testsSlotOfGetterWhereItStands(context.analyses, site, plan))
                readsOfFieldsThatMayBeEmpty.append({ site, WTF::move(*query), WTF::move(plan) });
            else if (testsPrototypeOfStoreWhereItStands(context, site, *query, plan))
                storesUnderPrototypesThatMayBeReplaced.append({ site, WTF::move(*query), WTF::move(plan) });
            else if (testsPrototypeOfReadWhereItStands(context, site, plan))
                readsUnderPrototypesThatMayBeReplaced.append({ site, WTF::move(*query), WTF::move(plan) });
            else if (!isRefusedForItsUses)
                refuse(plan.limit);
            return;
        }

        std::optional<ElementAccessSite> element = planElementAccessSite(context, site);
        if (element && element->plan.isResolved() && element->plan.needsNoTest())
            elementSites.append(WTF::move(*element));
        else if (element && canMarkIntegralKey(access, element->plan)) {
            // A form that needs tests is not rewritten here: the access stays by key and learns what the type of its key proves.
            ElementAccessPlan byKey;
            byKey.keyIsIntegral = true;
            elementSites.append({ site, WTF::move(byKey) });
        } else
            refuse();
    });
    bool hasSites = !namedSites.isEmpty() || !elementSites.isEmpty() || !readsOfFieldsThatMayBeEmpty.isEmpty() || !storesUnderPrototypesThatMayBeReplaced.isEmpty() || !readsUnderPrototypesThatMayBeReplaced.isEmpty();
    if (!hasSites && !didListStores)
        return PassOutcome::Unchanged;

    if (hasSites) {
        ProgramPoint current;
        // ScopedLambda passes its arguments on as lvalues.
        auto insertBeforeCurrent = [&](auto&& value) -> ValueID {
            builder.setOrigin(value.origin());
            return builder.insertBefore(current, WTF::move(value));
        };
        InsertValue insert = insertBeforeCurrent;
        AccessRewriter rewriter { context, insert };

        Vector<ValueID> rewritten;
        rewritten.reserveInitialCapacity(namedSites.size() + elementSites.size());
        for (const NamedAccessSite& site : namedSites) {
            current = site.site.point();
            rewriter.rewrite(site, operandsOfNamedAccess(function.at(site.site.value)));
            rewritten.append(site.site.value);
        }
        for (const ElementAccessSite& site : elementSites) {
            const Value& access = function.at(site.site.value);
            current = site.site.point();
            rewriter.rewrite(site, { access.objectOperand(), OperandPosition::value(access.opcode) ? access.valueOperand() : ValueID(), { } });
            if (site.plan.isResolved())
                rewritten.append(site.site.value);
        }
        builder.applyInsertions();
        context.analyses.didChangeValues();

        expandReadsOfFieldsThatMayBeEmpty(context, builder, readsOfFieldsThatMayBeEmpty);
        expandStoresUnderPrototypesThatMayBeReplaced(context, builder, storesUnderPrototypesThatMayBeReplaced);
        expandReadsUnderPrototypesThatMayBeReplaced(context, builder, readsUnderPrototypesThatMayBeReplaced);
        // Narrowing may renumber the values, and the planned sites name them.
        if (!rewritten.isEmpty())
            narrowValueTypes(TypePropagationInputs { program, context.id, function, context.analyses, context.types, context.build, &context.typingMemo }, rewritten.span());
    }
    // Each step tells the analyses of its edits as it makes them.
    context.keptAnalyses = allAnalyses;
    return PassOutcome::Changed;
}

static PropertyAccessPlan planWithoutCalls(FunctionPassContext& context, PropertyAccessQuery& query)
{
    query.isOnlyCalled = false;
    query.maxMembers = context.limit(LimitID::MaxAccessPlanMembers);
    return planPropertyAccess({ context.program, &context.inputs.flow }, query);
}

static const Value* findFilledSlotTest(std::span<const Value> tests)
{
    auto found = std::ranges::find(tests, Opcode::IsSlotFilled, &Value::opcode);
    return found == tests.end() ? nullptr : &*found;
}

// What the access is once its tests have passed: the receiver has one of the layouts that its test lists.
static PropertyAccessPlan planBehindTests(FunctionPassContext& context, PropertyAccessQuery& query, std::span<const Value> receiverTests)
{
    if (std::optional<ReceiverCandidates> established = receiversEstablishedBy(context.program, receiverTests))
        query.receivers = WTF::move(*established);
    query.expectsEmptySlot = (query.access == Access::Store || query.access == Access::Read) && !findFilledSlotTest(receiverTests);
    return planWithoutCalls(context, query);
}

// The rewrite plans the store again behind the tests. Whether what passes the test of the stored value fits the fields is settled here, by that plan, for the layouts that the test of the receiver lists.
static bool storedValueFitsBehindTests(FunctionPassContext& context, const PropertyAccessQuery& query, ValueID stored, const PlannedAccess& tested)
{
    if (!tested.plan.stores() || tested.plan.storedValueFits)
        return true;

    std::span<const Value> tests = tested.tests.tests.span();
    const Value& storedValueTest = tests.back();
    RELEASE_ASSERT(storedValueTest.opcode == Opcode::TypeTest && storedValueTest.operand(testedOperandIndex) == stored);

    Type passed = context.program.typeOf(context.function, stored).castTo(context.types.at(storedValueTest.testedType()), context.types);
    PropertyAccessQuery queryBehindTests = query;
    queryBehindTests.stored = &passed;
    PropertyAccessPlan plan = planBehindTests(context, queryBehindTests, tests.first(tests.size() - 1));
    return plan.isResolved() && plan.receiverTest == ReceiverTest::None && plan.stores() && plan.storedValueFits;
}

enum class ReceiversOfQuery : bool { AsExpected, Chosen };

static std::optional<PlannedAccess> planAccessUnderTests(TestSiteReporter& reporter, const Value& access, PropertyAccessQuery& query, ReceiversOfQuery receivers = ReceiversOfQuery::AsExpected)
{
    FunctionPassContext& context = reporter.context();
    AccessOperands operands = operandsOfNamedAccess(access);
    // The rule of calls has asked the same of a read or a store that it left alone.
    std::optional<PlannedAccess> planned;
    if (receivers == ReceiversOfQuery::AsExpected && !query.expectsEmptySlot && !query.mayTestSlotFirst && !query.storesWhatWasMadeOfField)
        planned = reporter.takePlanOfUse(false);
    while (true) {
        PlannedAccess tested;
        if (planned) {
            query.isOnlyCalled = false;
            query.maxMembers = context.limit(LimitID::MaxAccessPlanMembers);
            tested = WTF::move(*planned);
            planned = std::nullopt;
            if (tested.plan.needsNoTest())
                return std::nullopt;
        } else {
            tested.plan = planWithoutCalls(context, query);
            if (!tested.plan.isResolved() || tested.plan.needsNoTest())
                return std::nullopt;

            tested.stamp = createSlotStamp(context, access, tested.plan);
            tested.tests = createAccessTests(context, tested.plan, { operands.receiver, operands.stored, tested.stamp });
        }
        if (tested.tests.tests.isEmpty())
            return std::nullopt;

        const Value* receiverTest = tested.tests.findReceiverLayoutTest();
        if (receiverTest && reporter.findEarlierTest(*receiverTest) == EarlierTest::Failed) {
            size_t numberOfExpectedMembers = query.receivers.expected.predicted().size();
            query.receivers.stopExpecting(tested.plan.receivers);
            RELEASE_ASSERT(query.receivers.expected.predicted().size() < numberOfExpectedMembers);
            continue;
        }

        const Value* filledSlotTest = findFilledSlotTest(tested.tests.tests.span());
        if (filledSlotTest && query.access == Access::Store && !query.expectsEmptySlot && reporter.findEarlierTest(*filledSlotTest) == EarlierTest::Failed) {
            query.expectsEmptySlot = true;
            continue;
        }

        if (tested.plan.form == AccessForm::Call || !storedValueFitsBehindTests(context, query, operands.stored, tested))
            return std::nullopt;
        return tested;
    }
}

static std::optional<PlannedAccess> planForExpectedReceivers(TestSiteReporter& reporter, const Value& access, PropertyAccessQuery& query, LayoutList&& layouts)
{
    ReceiverCandidates expected;
    expected.level = KnowledgeLevel::Expected;
    expected.expected = WTF::move(layouts);
    expected.mayBeNullish = query.receivers.mayBeNullish;
    query.receivers = WTF::move(expected);
    return planAccessUnderTests(reporter, access, query, ReceiversOfQuery::Chosen);
}

// One test of a slot is cheaper than the three under which an empty slot reads as undefined.
AOT_THRESHOLD(unsigned, minOptionalFieldsReadToExpectEmptySlots, 4);

static Expected<LayoutSetID> layoutsOfReceiver(TestSiteReporter& reporter, const Value& access)
{
    return reporter.program().expectedOrTypeOf(reporter.function(), access.operand(*receiverOf(access.opcode))).predicted().layouts;
}

// `node.kind`, asked in every function of a program and in every round, of a family of some hundred layouts that all require it. The answer hangs on the set and the name alone.
static PropertyAccessCache::Optionality optionalityOf(const Program& program, LayoutSetID set, PropertyKeyID name)
{
    std::span<const LayoutID> layouts = program.at(set).completeMembers();
    auto compute = [&](PropertyAccessCache::DeclaredTypesKey key) {
        PropertyAccessCache::Optionality result { key, false, true };
        for (LayoutID layout : layouts) {
            std::optional<FieldReference> reference = findField(program, layout, name, BaseChain::Proven);
            bool isOptional = reference && !fieldAt(program, *reference).flags.contains(FieldFlags::IsRequired);
            result.isOptionalInSomeLayout |= isOptional;
            result.isOptionOfRecordInEveryLayout &= isOptional && !program.at(declaredLayoutOf(program, layout)).constructor;
        }
        return result;
    };
    PropertyAccessCache* cache = program.propertyAccessCache();
    if (!cache || layouts.size() < PropertyAccessCache::fewestLayouts || TypeTable::isProvisional(name))
        return compute({ });
    PropertyAccessCache::DeclaredTypesKey key { cache->lists.idOf(program.types, set), name };
    return findOrCompute(cache->optionalities, key, computeHash(key.layouts, key.key), [&] {
        return compute(key);
    });
}

// `const P = (F.prototype = {}); P.emit = ..`: the members of such a class are fields of a record, and the class lists none of them.
static bool claimsAbsenceOfNameOnInstalledPrototype(TestSiteReporter& reporter, const PropertyAccessPlan& plan, PropertyKeyID key)
{
    if (plan.member.kind != MemberKind::Absent && plan.member.kind != MemberKind::Field)
        return false;
    bool restsOnAbsence = std::ranges::any_of(plan.conditions, [&](const MemberCondition& condition) {
        return condition.kind == MemberCondition::Kind::MemberUnchanged && condition.key == key;
    });
    if (!restsOnAbsence)
        return false;
    const Program& program = reporter.program();
    const Heat& heat = reporter.context().inputs.heat;
    PropertyKeyID prototypeKey = program.keyOf(BuiltinNames::prototype);
    return std::ranges::any_of(program.layoutsWithName(key), [&](LayoutID layout) {
        return heat.isStoredUnder(layout, prototypeKey).predicted();
    });
}

// `Object.setPrototypeOf(make(), prototype).method()`: the object has just been handed a prototype, so a test that it still has the one of its layout fails every time and sends the rest of the function into its copy.
static bool testsPrototypeThatWasJustReplaced(TestSiteReporter& reporter, const Value& access, const PlannedAccess& tested)
{
    if (!std::ranges::contains(tested.tests.tests, Opcode::HasLayoutPrototype, &Value::opcode))
        return false;
    const Function& function = reporter.function();
    const Value& receiver = function.at(definitionThroughSameValues(function, access.operand(*receiverOf(access.opcode))));
    return receiver.opcode == Opcode::CallBuiltin && reporter.program().builtins().at(receiver.overload()).code.implementation.inlineBuiltin == InlineBuiltin::SetPrototypeOf;
}

// A test buys a load in place of a read by name and costs the test, the arm with the read by name and the join.
static bool isReadOfUntypedFieldInColdCode(TestSiteReporter& reporter, const Value& access, const PlannedAccess& tested)
{
    FunctionPassContext& context = reporter.context();
    const Function& function = reporter.function();
    BlockID block = reporter.point().block;
    if (!context.isBehindRounds || access.opcode != Opcode::PropGet || tested.plan.operation != Opcode::GetSlot || function.at(block).handler || heatClassOf(context, block).predicted() > HeatClass::Cold)
        return false;
    // What is entered most is small: accessors, selectors, callbacks. Their tests are few bytes. A body that was pasted in is as small as it was where it was written.
    if (std::min(function.values.size(), context.program.sourceFunctionOf(function, access.origin()).values.size()) <= HeatModel::maxSizeOfSmallFunction)
        return false;
    // However seldom the function is entered: a few calls may be hundreds of turns.
    if (context.analyses.naturalLoops().innerMostLoopOf(block))
        return false;
    const Value* receiverTest = tested.tests.findReceiverLayoutTest();
    return receiverTest && std::ranges::all_of(tested.tests.tests, [&](const Value& test) {
        return &test == receiverTest || (test.opcode == Opcode::IsSlotFilled && !test.holdsDeclaredType());
    });
}

// `this._events = ..` in EventEmitter, for every emitter of the program.
static std::optional<PlannedAccess> planForReceiversThatClaimNoAbsence(TestSiteReporter& reporter, const Value& access, PropertyAccessQuery& query, const PropertyAccessPlan& refused)
{
    if (query.start != MemberStart::Instance || refused.receivers.size() < 2)
        return std::nullopt;
    LayoutList others;
    for (LayoutID layout : refused.receivers) {
        PropertyAccessQuery queryOfOne = query;
        queryOfOne.receivers = ReceiverCandidates::underGuard(singleElementSpan(layout));
        PropertyAccessPlan plan = planWithoutCalls(reporter.context(), queryOfOne);
        if (plan.isResolved() && !claimsAbsenceOfNameOnInstalledPrototype(reporter, plan, query.key))
            others.add(layout);
    }
    if (others.isEmpty() || others.size() == refused.receivers.size())
        return std::nullopt;

    std::optional<PlannedAccess> tested = planForExpectedReceivers(reporter, access, query, WTF::move(others));
    if (!tested || claimsAbsenceOfNameOnInstalledPrototype(reporter, tested->plan, query.key))
        return std::nullopt;
    return tested;
}

// The layouts of the receiver are proven and find the name in several places, and cold code gets no arm for each place. The largest group is tested for, as it is where nothing is proven.
static std::optional<PlannedAccess> planForLargestGroupOfProvenReceivers(TestSiteReporter& reporter, const Value& access, PropertyAccessQuery& query)
{
    const ReceiverCandidates& receivers = query.receivers;
    if (query.start != MemberStart::Instance || !receivers.isProven() || receivers.hasOtherParts || receivers.proven.size() < 2)
        return std::nullopt;
    if (!isCold(heatClassOf(reporter.context(), reporter.point().block)).predicted())
        return std::nullopt;
    if (planWithoutCalls(reporter.context(), query).form != AccessForm::Split)
        return std::nullopt;
    return planForExpectedReceivers(reporter, access, query, LayoutList(receivers.proven));
}

// `const context = { cwd, depth }; context.file = () => load(context);`: two steps, so that the function can name the object. The literal stands in this function and does not list the field.
static bool storesFieldThatAllocationLeftOut(TestSiteReporter& reporter, const Value& store, PropertyKeyID key)
{
    const Program& program = reporter.program();
    const Value& allocation = reporter.function().at(reporter.analyses().identityOf(store.operand(*receiverOf(store.opcode))));
    if (allocation.opcode != Opcode::NewObject)
        return false;
    std::optional<uint32_t> field = program.at(declaredLayoutOf(program, allocation.layout())).findField(key);
    return field && !std::ranges::contains(allocation.fields(), *field);
}

static bool storesWhatWasMadeOfField(const Program& program, const Function& function, const Value& store, PropertyKeyID key)
{
    static constexpr size_t maxValuesWalked = 8;
    RootThroughCasts object = sameObjectThroughCasts(function, store.operand(*receiverOf(store.opcode)));
    Vector<ValueID, maxValuesWalked> walked;
    walked.append(store.operand(*storedOf(store.opcode)));
    for (size_t next = 0; next < walked.size(); ++next) {
        const Value& value = function.at(walked[next]);
        if (value.opcode == Opcode::PropGet || value.opcode == Opcode::GetSlot) {
            PropertyKeyID read = value.opcode == Opcode::PropGet ? keyOfNamedAccess(program, value) : value.key();
            if (read == key && sameObjectThroughCasts(function, value.objectOperand()) == object)
                return true;
        }
        value.forEachOperand([&](ValueID operand, uint32_t) {
            if (operand && walked.size() < maxValuesWalked && !walked.contains(operand))
                walked.append(operand);
        });
    }
    return false;
}

// For each set of layouts the optional fields that the likely code of the function reads off it, each name once. A function that walks the keys of such an object reads them all.
static UncheckedKeyHashMap<uint32_t, unsigned> countOptionalFieldsRead(TestSiteReporter& reporter)
{
    const Program& program = reporter.program();
    const Function& function = reporter.function();
    UncheckedKeyHashSet<uint64_t> seen;
    UncheckedKeyHashMap<uint32_t, unsigned> counts;
    for (BlockID block : reporter.analyses().cfg().reversePostOrder()) {
        if (function.at(block).frequency.predicted() != BlockFrequency::Normal)
            continue;
        for (ValueID id : function.at(block).values) {
            const Value& value = function.at(id);
            // `{ wrap: "nowrap", ...style }`, `const { children, ...rest } = props`: what is copied key by key, whichever keys it has. An arm for one order of its keys asks for that order.
            ValueID walked;
            if (value.opcode == Opcode::CopyDataProperties)
                walked = value.operand(OperandIndex::CopyDataProperties::source);
            else if (value.opcode == Opcode::HasKeyOrder)
                walked = value.operand(OperandIndex::HasKeyOrder::object);
            if (walked) {
                if (LayoutSetID layouts = program.expectedOrTypeOf(function, walked).predicted().layouts)
                    counts.add(layouts.index() + 1, 0).iterator->value += minOptionalFieldsReadToExpectEmptySlots;
                continue;
            }
            bool readsField = value.opcode == Opcode::PropGet || value.opcode == Opcode::GetSlot || value.opcode == Opcode::GetSlotOrUndefined;
            if (!readsField || !value.key())
                continue;
            LayoutSetID layouts = layoutsOfReceiver(reporter, value).predicted();
            if (!layouts || !seen.add((static_cast<uint64_t>(layouts.index()) + 1) << 32 | value.key().index()).isNewEntry)
                continue;
            if (optionalityOf(program, layouts, value.key()).isOptionalInSomeLayout)
                counts.add(layouts.index() + 1, 0).iterator->value++;
        }
    }
    return counts;
}

// In a checked function of the program an option is what the checker says it is. The records of the library hold what callers hand in, and their empty slots mostly fail one of the tests as well.
static bool readsOptionsLeftOutAsUndefined(const FunctionPassContext& context)
{
    return context.function.has(FunctionFlags::IsChecked) && !isLibraryCode(context.program, context.function);
}

// Whoever reports the tests of a read and whoever rewrites the read behind them ask the same.
static bool mayTestSlotFirst(const FunctionPassContext& context, const PropertyAccessQuery& query)
{
    return query.access == Access::Read && readsOptionsLeftOutAsUndefined(context) && !isCold(context.inputs.heat.of(context.program, context.id)).predicted();
}

void reportPropertyAccessTests(TestSiteReporter& reporter)
{
    std::optional<UncheckedKeyHashMap<uint32_t, unsigned>> optionalFieldsRead;
    reporter.forEachCandidate([&](const Value& access) {
        std::optional<PropertyAccessQuery> query = propertyAccessQueryOf(reporter.program(), reporter.function(), access, ExpectedClassObjects::Tested);
        if (!query)
            return;
        bool mayMeetOptionLeftOut = query->access == Access::Read || query->access == Access::Store;
        if (LayoutSetID layouts = mayMeetOptionLeftOut ? layoutsOfReceiver(reporter, access).predicted() : LayoutSetID()) {
            if (!optionalFieldsRead)
                optionalFieldsRead = countOptionalFieldsRead(reporter);
            bool handlesOptions = optionalFieldsRead->get(layouts.index() + 1) >= minOptionalFieldsReadToExpectEmptySlots;
            // `style.wrap ??= "nowrap"`: the read has taken the empty slot for undefined, so no test of the slot has failed in front of the store that fills it.
            query->expectsEmptySlot = handlesOptions && (query->access == Access::Read || optionalityOf(reporter.program(), layouts, query->key).isOptionOfRecordInEveryLayout);
        }
        if (query->access == Access::Store && storesFieldThatAllocationLeftOut(reporter, access, query->key))
            query->expectsEmptySlot = true;
        query->mayTestSlotFirst = mayTestSlotFirst(reporter.context(), *query);
        query->storesWhatWasMadeOfField = query->access == Access::Store && storesWhatWasMadeOfField(reporter.program(), reporter.function(), access, query->key);
        std::optional<PlannedAccess> tested = planAccessUnderTests(reporter, access, *query);
        if (!tested)
            tested = planForLargestGroupOfProvenReceivers(reporter, access, *query);
        if (tested && claimsAbsenceOfNameOnInstalledPrototype(reporter, tested->plan, query->key)) {
            PropertyAccessPlan refused = WTF::move(tested->plan);
            tested = planForReceiversThatClaimNoAbsence(reporter, access, *query, refused);
        }
        if (!tested || testsPrototypeThatWasJustReplaced(reporter, access, *tested) || isReadOfUntypedFieldInColdCode(reporter, access, *tested))
            return;
        if (tested->plan.member.kind == MemberKind::Absent && access.opcode == Opcode::PropGet && isProvenToFindFunctions(reporter.context(), reporter.use()))
            return;
        const Value* receiverTest = tested->tests.findReceiverLayoutTest();
        if (receiverTest && leavesTestOfPrototypeToStore(reporter.context(), access, *query, tested->plan) && isWorthArms(reporter.context(), reporter.point().block)) {
            // Once the layouts of the receiver are proven, ResolveAccesses gives the store its arms.
            reporter.report({ *receiverTest }, reporter.analyses().effectsOf(reporter.use()).normal);
            return;
        }
        reporter.report(tested->tests.tests.span(), effectsAfterRewrite(reporter.context(), tested->plan, tested->stamp));
    });
}

static bool isGuardValidation(const Function& function, const UseLists& useLists, const Block& block, uint32_t index)
{
    if (index + 2 >= block.values.size())
        return false;
    ValueID test = block.values[index];
    ValueID expected = block.values[index + 1];
    const Value& validation = function.at(block.values[index + 2]);
    if (provesOf(function.at(test).opcode) == Proof::None || function.at(expected).opcode != Opcode::Boolean || validation.opcode != Opcode::ValidateSameValue)
        return false;
    return validation.operand(0) == test && validation.operand(1) == expected && useLists.useCount(test) == 1 && useLists.useCount(expected) == 1;
}

static std::optional<ValueSite> findAccessBehindSlotTest(FunctionPassContext& context, const Value& test, BlockID filled)
{
    const Function& function = context.function;
    const UseLists& useLists = context.analyses.useLists();
    if (function.at(filled).parameters.size() != 1 || useLists.useCount(function.at(filled).parameters[0]) != 1)
        return std::nullopt;
    const Terminator& terminator = function.at(filled).terminator;
    if (function.at(filled).values.isEmpty() && terminator.kind == TerminatorKind::Jump && terminator.targets[0].arguments.isEmpty()) {
        filled = terminator.targets[0].block;
        if (!function.at(filled).parameters.isEmpty() || !context.analyses.cfg().singleIncomingEdge(filled))
            return std::nullopt;
    }
    const Block& block = function.at(filled);
    for (uint32_t index = 0; index < block.values.size(); ++index) {
        ValueID id = block.values[index];
        const Value& value = function.at(id);
        if (value.opcode == Opcode::AllGuards && useLists.useCount(id) == 1)
            continue;
        if (isGuardValidation(function, useLists, block, index)) {
            index += 2;
            continue;
        }
        if (value.opcode != Opcode::GetSlot && value.opcode != Opcode::PutSlot)
            return std::nullopt;
        if (value.objectOperand() != test.objectOperand() || value.key() != test.key())
            return std::nullopt;
        if (value.opcode == Opcode::PutSlot && std::ranges::contains(block.values.span().first(index), value.valueOperand()))
            return std::nullopt;
        if (value.opcode == Opcode::GetSlot && value.holdsDeclaredType())
            return std::nullopt;
        return ValueSite { filled, index, id };
    }
    return std::nullopt;
}

namespace {

struct AccessBehindSlotTest {
    BlockID test;
    ValueSite access;
};

} // anonymous namespace

static Vector<AccessBehindSlotTest> findAccessesBehindSlotTests(FunctionPassContext& context)
{
    const Function& function = context.function;
    const CFG& cfg = context.analyses.cfg();
    Vector<AccessBehindSlotTest> sites;
    for (BlockID id : cfg.reversePostOrder()) {
        const Block& block = function.at(id);
        if (block.frequency.predicted() != BlockFrequency::Normal || block.values.isEmpty() || !block.terminator.isConditionalBranch())
            continue;
        const Value& test = function.at(block.values.last());
        if (test.opcode != Opcode::IsSlotFilled || test.holdsDeclaredType() || block.terminator.conditionOperand() != block.values.last())
            continue;
        const Target& filled = block.terminator.taken();
        bool leavesWhereEmpty = isRarerThan(function.at(block.terminator.notTaken().block).frequency, block.frequency).predicted();
        if (!leavesWhereEmpty || !filled.arguments.isEmpty() || !cfg.singleIncomingEdge(filled.block))
            continue;
        if (std::optional<ValueSite> access = findAccessBehindSlotTest(context, test, filled.block))
            sites.append({ id, *access });
    }
    return sites;
}

namespace {

struct EmptySlotCases {
    // More than one where the receivers have several methods of the name: each plan lists the receivers that have its method.
    Vector<PropertyAccessPlan, 1> plans;
    // The last plan is for all that the others leave.
    bool lastTakesRest { true };
};

} // anonymous namespace

// Outside the cell a field takes no room while it holds undefined, so the test of its slot finds nothing.
static bool mayHoldUndefinedWithoutStorage(const Program& program, const SlotPlan& slots, const Value& slotTest)
{
    return std::ranges::any_of(program.at(slotTest.receivers()).completeMembers(), [&](LayoutID member) {
        std::optional<uint32_t> field = program.at(member).findField(slotTest.key());
        return field && slots.slot(member, *field).isOutOfLine() && slots.slot(member, *field).mayExistWithoutStorage;
    });
}

static EmptySlotCases planCaseOfFieldWithoutStorage(FunctionPassContext& context, const Value& slotTest, const Type& typeOfRead)
{
    if (!(typeOfRead.primitives & Primitive::Undefined) || !mayHoldUndefinedWithoutStorage(context.program, *context.inputs.slots, slotTest))
        return { };
    std::optional<TestsOfEmptySlot> needed = testsThatOptionLeftOutReadsAsUndefined({ context.program, &context.inputs.flow }, context.program.at(slotTest.receivers()).completeMembers(), slotTest.key(), OptionKind::FieldWithoutStorage);
    if (!needed)
        return { };
    PropertyAccessPlan plan;
    plan.requiredProofs = needed->proofs;
    plan.conditions = WTF::move(needed->conditions);
    return { { WTF::move(plan) }, true };
}

static EmptySlotCases planEmptySlotCases(FunctionPassContext& context, const Value& slotTest, const Type& typeOfRead, bool isStore, bool functionStoresField)
{
    PropertyAccessContext accessContext { context.program, &context.inputs.flow };
    std::span<const LayoutID> receivers = context.program.at(slotTest.receivers()).completeMembers();
    bool handlesLazyFields = !isLibraryCode(context.program, context.function);
    PropertyAccessPlan plan;
    if (isStore) {
        std::optional<LazyFieldCreation> creation = handlesLazyFields ? planLazyFieldCreation(accessContext, receivers, slotTest.key()) : std::nullopt;
        if (!creation)
            return { };
        plan.operation = creation->operation;
        plan.storedValueFits = true;
        plan.requiredProofs = creation->proofs;
        plan.conditions = WTF::move(creation->conditions);
        return { { WTF::move(plan) }, true };
    }
    OptionSet<OptionKind> kinds;
    if (readsOptionsLeftOutAsUndefined(context))
        kinds.add(OptionKind::RecordOption);
    if (handlesLazyFields && functionStoresField)
        kinds.add(OptionKind::LazilyInitializedField);
    bool admitsUndefined = !!(typeOfRead.primitives & Primitive::Undefined);
    std::optional<TestsOfEmptySlot> needed = kinds.isEmpty() || !admitsUndefined ? std::nullopt : testsThatOptionLeftOutReadsAsUndefined(accessContext, receivers, slotTest.key(), kinds);
    if (needed) {
        plan.requiredProofs = needed->proofs;
        plan.conditions = WTF::move(needed->conditions);
        return { { WTF::move(plan) }, true };
    }
    MethodsAboveEmptySlot above = findMethodsAboveEmptySlot(accessContext, receivers, slotTest.key());
    EmptySlotCases cases { { }, above.areAllReceivers };
    // The classes of the module in which the read is written come first.
    ModuleID module = context.function.module;
    std::ranges::stable_partition(above.methods, [&](const MethodAboveEmptySlot& method) {
        return std::ranges::any_of(method.layouts, [&](LayoutID layout) {
            return context.program.at(layout).module == module;
        });
    });
    for (MethodAboveEmptySlot& method : above.methods) {
        if (cases.plans.size() == context.limit(LimitID::MaxDispatchCases) || !Type::forFunction(method.method).isSubsetOf(typeOfRead, context.program)) {
            cases.lastTakesRest = false;
            continue;
        }
        PropertyAccessPlan planOfMethod;
        planOfMethod.form = AccessForm::Method;
        planOfMethod.member.method = method.method;
        planOfMethod.receivers = WTF::move(method.layouts);
        planOfMethod.requiredProofs = method.proofs;
        planOfMethod.conditions = WTF::move(method.conditions);
        cases.plans.append(WTF::move(planOfMethod));
    }
    return cases;
}

static void addEmptySlotCase(FunctionPassContext& context, IRBuilder& builder, SiteExpansion& expansion, const AccessBehindSlotTest& site, const Value& slotTest, const EmptySlotCases& cases, bool isStore)
{
    std::span<const PropertyAccessPlan> plans = cases.plans.span();
    Function& function = context.function;
    Expected<BlockFrequency> frequency = function.at(site.test).frequency;
    Target leaves = function.at(site.test).terminator.notTaken();
    expansion.begin(site.access);
    Value original = expansion.original();
    ValueID copy = expansion.appendOriginal();
    if (isStore)
        expansion.finishArmWithoutResult();
    else
        expansion.finishArm(copy);

    BlockID empty = builder.addBlock(frequency);
    builder.moveToEndOf(empty);
    builder.setOrigin(slotTest.origin());
    for (const PropertyAccessPlan& plan : plans) {
        BlockID others;
        bool isLast = &plan == &plans.back();
        if (!isLast || !cases.lastTakesRest) {
            ValueID hasMethod = builder.append(makeTypeTest(slotTest.objectOperand(), context.types.intern(Type::forLayouts(plan.receivers.span(), context.types))));
            BlockID mine = builder.addBlock(frequency);
            if (!isLast)
                others = builder.addBlock(frequency);
            builder.emitBranch(hasMethod, Target { mine, { }, { } }, isLast ? Target(leaves) : Target { others, { }, { } });
            builder.moveToEndOf(mine);
        }
        AccessTests tests = createAccessTests(context, plan, { slotTest.objectOperand(), { }, { slotTest.key(), slotTest.receivers(), slotTest.level(), { } } });
        Vector<ValueID, 4> guards;
        if (slotTest.guardOperand())
            guards.append(slotTest.guardOperand());
        for (Value& test : tests.tests) {
            if (slotTest.guardOperand() && OperandPosition::guard(test.opcode))
                test.setGuardOperand(slotTest.guardOperand());
            ValueID passed = builder.append(WTF::move(test));
            if (isStore) {
                IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(passed, 0, frequency, frequency, { { }, Target(leaves) });
                guards.append(branch.provenOnTaken);
                builder.moveToEndOf(branch.taken);
                continue;
            }
            BlockID next = builder.addBlock(frequency);
            builder.emitBranch(passed, Target { next, { }, { } }, Target(leaves));
            builder.moveToEndOf(next);
        }
        if (isStore) {
            Value store(plan.operation);
            store.setObjectOperand(original.objectOperand());
            if (!guards.isEmpty())
                store.setGuardOperand(guards.size() == 1 ? guards[0] : builder.appendAllGuards(guards.span()));
            store.setValueOperand(original.valueOperand());
            setSlotStamp(store, { original.key(), original.receivers(), original.level(), original.accessSite() });
            builder.append(WTF::move(store));
            expansion.finishArmWithoutResult();
        } else if (plan.form == AccessForm::Method) {
            // Whoever compares it with another read of the member finds the same closure.
            Value closure = IRBuilder::make(Opcode::NewClosure);
            closure.setFunction(plan.member.method);
            closure.setIsHeldByClosureTable(true);
            expansion.finishArm(expansion.append(WTF::move(closure)));
        } else
            expansion.finishArm(expansion.append(IRBuilder::make(Opcode::Undefined)));
        if (others)
            builder.moveToEndOf(others);
    }
    expansion.end();
    builder.redirectEdge(Edge { site.test, 1 }, Target { empty, { }, { } });
    // A field that lies out of line may hold undefined without storage. It hides the method above it, so the test has to see it.
    if (std::ranges::contains(plans, AccessForm::Method, &PropertyAccessPlan::form))
        function.edit(function.at(site.test).values.last(), ValueEdit::Definition).setCountsFieldsWithoutStorage(true);
}

PassOutcome addEmptySlotCases(FunctionPassContext& context)
{
    Vector<AccessBehindSlotTest> sites = findAccessesBehindSlotTests(context);
    if (sites.isEmpty())
        return PassOutcome::Unchanged;

    Function& function = context.function;
    UncheckedKeyHashSet<uint32_t> storedKeys;
    for (const AccessBehindSlotTest& site : sites) {
        const Value& access = function.at(site.access.value);
        if (access.opcode == Opcode::PutSlot)
            storedKeys.add(access.key().index() + 1);
    }

    IRBuilder builder({ context.build, context.program, &context.inputs.flow, &context.inputs.effects, context.id }, function, context.types, &context.analyses);
    SiteExpansion expansion(context, builder);
    PassOutcome outcome = PassOutcome::Unchanged;
    // An expansion moves what stands behind the access into a block of its own, the next slot test with it. So the last comes first.
    for (const AccessBehindSlotTest& site : sites | std::views::reverse) {
        Value slotTest = function.at(function.at(site.test).values.last());
        bool isStore = function.at(site.access.value).opcode == Opcode::PutSlot;
        EmptySlotCases cases = planEmptySlotCases(context, slotTest, context.program.typeOf(function, site.access.value), isStore, storedKeys.contains(slotTest.key().index() + 1));
        if (cases.plans.isEmpty())
            continue;
        addEmptySlotCase(context, builder, expansion, site, slotTest, cases, isStore);
        outcome = PassOutcome::Changed;
    }
    return outcome;
}

// Only the plan of the slots says which fields these are.
PassOutcome addCasesOfFieldsWithoutStorage(FunctionPassContext& context)
{
    Function& function = context.function;
    Vector<AccessBehindSlotTest> sites = findAccessesBehindSlotTests(context);
    sites.removeAllMatching([&](const AccessBehindSlotTest& site) {
        return function.at(site.access.value).opcode != Opcode::GetSlot;
    });
    if (sites.isEmpty())
        return PassOutcome::Unchanged;

    IRBuilder builder({ context.build, context.program, &context.inputs.flow, &context.inputs.effects, context.id }, function, context.types, &context.analyses);
    SiteExpansion expansion(context, builder);
    PassOutcome outcome = PassOutcome::Unchanged;
    for (const AccessBehindSlotTest& site : sites | std::views::reverse) {
        Value slotTest = function.at(function.at(site.test).values.last());
        EmptySlotCases cases = planCaseOfFieldWithoutStorage(context, slotTest, context.program.typeOf(function, site.access.value));
        if (cases.plans.isEmpty())
            continue;
        addEmptySlotCase(context, builder, expansion, site, slotTest, cases, false);
        outcome = PassOutcome::Changed;
    }
    return outcome;
}

static bool testsCoverPlan(std::span<const Value> tests, const PropertyAccessPlan& plan, PropertyKeyID key)
{
    auto findsWhatPlanExpectsInSlot = [&](const Value& test) {
        return test.key() == key && (plan.stores() || !plan.testsDeclaredType || test.holdsDeclaredType());
    };
    bool coverProofs = std::ranges::all_of(allMembersOf<Proof>, [&](Proof proof) {
        return !plan.requiredProofs.contains(proof) || std::ranges::any_of(tests, [&](const Value& test) {
            return provesOf(test.opcode) == proof && OperandPosition::object(test.opcode) && (proof != Proof::SlotFilled || findsWhatPlanExpectsInSlot(test));
        });
    });
    return coverProofs && testsCoverConditions(tests, plan.conditions.span());
}

// An object stored into itself also passed the test of the stored value, so its type may list fewer layouts than its own test did. Effects go by that type.
static void narrowPlanToReceiverType(FunctionPassContext& context, std::span<const Value> tests, ValueID receiver, PropertyAccessQuery& query, PropertyAccessPlan& plan)
{
    ReceiverCandidates byType = candidatesOf(context.program, context.program.typeOf(context.function, receiver), nullptr, { });
    if (byType.proof != ReceiverProof::Type || byType.hasOtherParts)
        return;
    LayoutList layouts = plan.receivers;
    layouts.intersect(byType.proven);
    if (layouts.isEmpty() || layouts.size() == plan.receivers.size())
        return;

    PropertyAccessQuery narrowed = query;
    narrowed.receivers = ReceiverCandidates::underGuard(layouts.span());
    // Effects are computed without flow results, so that plan is tried too.
    for (const FlowResults* flow : { &context.inputs.flow, static_cast<const FlowResults*>(nullptr) }) {
        PropertyAccessPlan narrower = planPropertyAccess({ context.program, flow }, narrowed);
        if (!narrower.isResolved() || narrower.receiverTest != ReceiverTest::None || !testsCoverPlan(tests, narrower, query.key))
            continue;
        query = WTF::move(narrowed);
        plan = WTF::move(narrower);
        return;
    }
}

void rewriteTestedPropertyAccess(TestSiteRewriter& rewriter)
{
    FunctionPassContext& context = rewriter.context();
    const Program& program = rewriter.program();
    const Value& access = rewriter.useValue();

    std::optional<PropertyAccessQuery> query = propertyAccessQueryOf(program, rewriter.function(), access, ExpectedClassObjects::Tested);
    RELEASE_ASSERT(query);
    query->mayTestSlotFirst = mayTestSlotFirst(context, *query);
    query->storesWhatWasMadeOfField = query->access == Access::Store && findFilledSlotTest(rewriter.tests());

    AccessOperands operands = operandsOfNamedAccess(access);
    operands.guard = rewriter.guard();

    // A test of the stored value may name layouts too, and it says nothing about the receiver. The operand of the site is what passed the test, or a cast of it to the type the operand had.
    std::span<const Value> receiverTests = rewriter.tests();
    if (operands.stored && !receiverTests.empty()) {
        uint32_t last = static_cast<uint32_t>(receiverTests.size() - 1);
        if (receiverTests[last].opcode == Opcode::TypeTest && std::ranges::contains(CastChain(rewriter.function(), operands.stored), rewriter.proofOf(last)))
            receiverTests = receiverTests.first(last);
    }
    // The receiver is by now what passed its test, and no cast leads from there to the allocation. The test still names what the report asked about.
    if (!receiverTests.empty() && receiverTests[0].opcode == Opcode::TypeTest)
        query->isListedByAllocation = isAllocationListing(program, rewriter.function(), receiverTests[0].operand(testedOperandIndex), query->key);

    PropertyAccessPlan plan = planBehindTests(context, *query, receiverTests);
    RELEASE_ASSERT(plan.isResolved());
    RELEASE_ASSERT(plan.receiverTest == ReceiverTest::None);
    if (plan.requiredProofs.contains(Proof::PrototypeLinkUnchanged) && !std::ranges::contains(rewriter.tests(), Opcode::HasLayoutPrototype, &Value::opcode))
        return;
    ASSERT(testsCoverPlan(rewriter.tests(), plan, query->key));
    narrowPlanToReceiverType(context, rewriter.tests(), operands.receiver, *query, plan);
    plan.level = KnowledgeLevel::Expected;

    if (plan.stores() && !plan.storedValueFits) {
        // One value stands for everything that has the identity of the tested value, so its type may be wider than that of the operand the site was planned with.
        TypeArena& types = context.types;
        Type fieldType = storedTypeOf(context.program, plan.receivers.span(), query->key, types);
        Type narrowed = context.program.typeOf(context.function, operands.stored).intersectWith(fieldType, types);
        // No value is both what the operand may be and what the fields hold, so nothing gets here.
        if (narrowed.isNever())
            return;

        TypeID castType = types.intern(narrowed);
        if (context.build.validates())
            rewriter.insertBeforeUse(makeValidateType(operands.stored, castType));
        operands.stored = rewriter.insertBeforeUse(makeAssumeType(operands.stored, Proven::byRulesWithoutChecks(castType)));
        plan.storedValueFits = true;
    }

    // ScopedLambda hands its arguments to the functor as lvalues.
    auto insertBeforeUse = [&](auto&& value) -> ValueID {
        return rewriter.insertBeforeUse(WTF::move(value));
    };
    InsertValue insert(insertBeforeUse);
    ProgramPoint point = rewriter.point();
    AccessRewriter { context, insert }.rewrite(NamedAccessSite { { point.block, point.index, rewriter.use() }, WTF::move(*query), WTF::move(plan) }, operands);
}

// Binding drops the global object operand and keeps the type and the position of the access. None of the four opcodes is a cast, a test or a constant.
static constexpr AnalysisSet analysesUnaffectedByBinding { FunctionAnalysis::Identities, FunctionAnalysis::Frequencies, FunctionAnalysis::FailedTests };

static constexpr bool isNeitherCastNorTest(Opcode opcode)
{
    return !isCastOrConversion(opcode) && provesOf(opcode) == Proof::None;
}

static_assert(isNeitherCastNorTest(Opcode::PropGet) && isNeitherCastNorTest(Opcode::PropSet) && isNeitherCastNorTest(Opcode::GetGlobalBinding) && isNeitherCastNorTest(Opcode::SetGlobalBinding));

static void becomeAccessOfSameName(Value& access, Opcode read, Opcode store)
{
    PropertyKeyID key = access.key();
    AccessSiteID site = access.accessSite();
    if (OperandPosition::value(access.opcode)) {
        ECMAMode mode = access.mode();
        ValueID stored = access.valueOperand();
        access.become(store);
        access.setMode(mode);
        access.setValueOperand(stored);
    } else {
        bool isForTypeOf = access.isForTypeOf();
        access.become(read);
        access.setIsForTypeOf(isForTypeOf);
    }
    access.setKey(key);
    access.setAccessSite(site);
}

void convertGlobalBindingToNamedAccess(IRBuilder& builder, ProgramPoint point, ValueID id)
{
    Function& function = builder.function();
    RELEASE_ASSERT(globalBindingAccesses.contains(function.at(id).opcode));

    builder.setOrigin(function.at(id).origin());
    Value globalObject = IRBuilder::make(Opcode::Builtin);
    globalObject.setBuiltin(Builtins::GlobalThis);
    ValueID object = builder.insertBefore(point, WTF::move(globalObject));

    Value& access = function.edit(id);
    bool isFreeName = access.seesScriptBindings();
    becomeAccessOfSameName(access, Opcode::PropGet, Opcode::PropSet);
    access.setObjectOperand(object);
    access.setIsFreeName(isFreeName);
}

PassOutcome bindGlobalNames(FunctionPassContext& context)
{
    const Program& program = context.program;
    Function& function = context.function;
    const BuiltinTable& table = program.builtins();
    Vector<ValueID, 8> accesses;
    for (const ValueSite& site : context.sitesIn(SiteList::GlobalNames)) {
        const Value& access = function.at(site.value);
        BuiltinID receiver = singleBuiltinOf(program.typeOf(function, access.objectOperand()));
        if (!receiver || table.canonicalID(receiver) != table.canonicalID(Builtins::GlobalThis) || keyLookupOf(program, access.key()) != KeyLookup::Ordinary)
            continue;
        if (freeNameAnswerOf({ program, context.inputs.flow }, access.key(), access.isFreeName()).kind == FreeNameAnswerKind::Bound)
            accesses.append(site.value);
    }
    if (accesses.isEmpty())
        return PassOutcome::Unchanged;

    for (ValueID id : accesses) {
        Value& access = function.edit(id);
        bool seesScriptBindings = access.isFreeName();
        becomeAccessOfSameName(access, Opcode::GetGlobalBinding, Opcode::SetGlobalBinding);
        access.setLevel(KnowledgeLevel::Expected);
        access.setSeesScriptBindings(seesScriptBindings);
    }
    context.analyses.invalidate(valueAnalyses - analysesUnaffectedByBinding);
    context.keptAnalyses = analysesUnaffectedByBinding;
    return PassOutcome::Changed;
}

void forEachValueSite(FunctionAnalyses& analyses, const Function& function, NOESCAPE const ScopedLambda<void(const ValueSite&)>& functor)
{
    for (BlockID block : analyses.cfg().reversePostOrder()) {
        const BlockValues& values = function.at(block).values;
        for (uint32_t index = 0; index < values.size(); ++index)
            functor(ValueSite { block, index, values[index] });
    }
}

void checkRewrite(FunctionPassContext& context, const ValueSite& site, const ReplacedEffects& replaced)
{
    if (!context.build.options().verifiesProgram)
        return;
    const Value& value = context.function.at(site.value);
    if (replacedOperationsOf(value.opcode).empty())
        return;
    checkReplacement({ context.program, context.inputs.effects }, context.function, { context.id, site.block, site.value }, value, replaced, context.build.diagnostics());
}

void SiteExpansion::begin(const ValueSite& site)
{
    RELEASE_ASSERT(!isOpen());
    RELEASE_ASSERT(function().at(site.block).values[site.index] == site.value);

    m_site = site;
    m_original = function().at(site.value);
    m_frequency = function().at(site.block).frequency;
    m_continuationPredecessors.clear();
    m_operationsBeforeOriginal.clear();
    m_operandIndexInOriginal = 0;
    m_appended.clear();
    m_replaced = { };
    addReplaced(m_original);
    explainOperationThatGoes(function(), m_site.value, InPlaceOfOperation::ItsExpansion);

    m_expansion = m_builder.beginExpansion(site.point());
    Block& continuation = function().edit(m_expansion.continuation);
    if (continuation.terminator.kind != TerminatorKind::ReturnCall || continuation.terminator.callOperand() != m_site.value)
        return;

    // Each arm returns on its own, so nothing enters the continuation.
    m_tailCall = WTF::move(continuation.terminator);
    continuation = Block { };
    becomeUndefined(function(), m_site.value);
    m_context.analyses.didChangeControlFlow();
}

void SiteExpansion::end()
{
    RELEASE_ASSERT(isOpen() && !m_builder.currentBlock());
    for (ValueID id : m_appended)
        checkRewrite(m_context, ValueSite { m_site.block, m_site.index, id }, m_replaced);
    if (m_continuationPredecessors.size() == 1)
        m_builder.mergeContinuation(m_expansion, m_continuationPredecessors[0]);
    m_builder.setHandler(std::nullopt);
    m_tailCall = std::nullopt;
    m_site = { };
}

void SiteExpansion::addReplaced(const Value& value)
{
    if (m_context.build.options().verifiesProgram)
        m_replaced.add({ m_context.program, m_context.inputs.effects }, function(), value);
}

void SiteExpansion::setOperationsBeforeOriginal(Vector<Value, 2>&& values, uint32_t operandIndexInOriginal)
{
    RELEASE_ASSERT(isOpen());
    m_operationsBeforeOriginal = WTF::move(values);
    m_operandIndexInOriginal = operandIndexInOriginal;
}

ValueID SiteExpansion::append(Value&& value)
{
    ValueID id = m_builder.append(WTF::move(value));
    m_appended.append(id);
    return id;
}

static ValueID appendCopyInArm(IRBuilder& builder, Value&& copy)
{
    if (resultRuleOf(copy.opcode) == ResultRule::Declared) {
        TypeID type = copy.type;
        return builder.appendWithType(WTF::move(copy), type);
    }
    // A narrower operand may narrow the type, and the expected type has to lie strictly inside the type: the builder computes both again.
    copy.setExpected({ });
    return builder.append(WTF::move(copy));
}

ValueID SiteExpansion::appendOriginal(const OriginalCopy& copy)
{
    RELEASE_ASSERT(isOpen());
    Value value = m_original;
    Origin originOfOriginal = m_builder.origin();
    ValueID previous;
    for (const Value& operation : m_operationsBeforeOriginal) {
        Value before = operation;
        if (previous)
            before.setOperand(0, previous);
        else {
            if (copy.operand) {
                std::optional<uint8_t> receiver = receiverOf(before.opcode);
                RELEASE_ASSERT(receiver);
                before.setOperand(*receiver, copy.operand);
            }
            if (copy.receivers && PayloadWord::receivers(before.opcode))
                before.setReceivers(copy.receivers);
        }
        m_builder.setOrigin(operation.origin());
        previous = appendCopyInArm(m_builder, WTF::move(before));
    }
    bool hasOperationsBefore = !m_operationsBeforeOriginal.isEmpty();
    if (hasOperationsBefore) {
        m_builder.setOrigin(originOfOriginal);
        value.setOperand(m_operandIndexInOriginal, previous);
    }
    if (copy.operandIndex) {
        ValueID replaced = value.operand(*copy.operandIndex);
        value.setOperand(*copy.operandIndex, copy.operand);
        if (copy.isNarrowedValue) {
            value.forEachOperand([&](ValueID& operand, uint32_t) {
                if (operand == replaced)
                    operand = copy.operand;
            });
        }
    }
    if (copy.receivers && !hasOperationsBefore && PayloadWord::receivers(value.opcode))
        value.setReceivers(copy.receivers);
    if (copy.callees && PayloadWord::callees(value.opcode))
        value.setCallees(copy.callees);
    return appendCopyInArm(m_builder, WTF::move(value));
}

void SiteExpansion::finishArm(ValueID result)
{
    RELEASE_ASSERT(isOpen());
    if (result && m_context.program.typeOf(function(), result).isNever()) {
        finishArmAsUnreachable();
        return;
    }
    if (m_tailCall) {
        finishArmWithReturn(result);
        return;
    }
    if (m_expansion.results.isEmpty()) {
        finishArmWithoutResult();
        return;
    }

    RELEASE_ASSERT(result && m_expansion.results.size() == 1);
    ValueID converted = convertTo(m_builder, result, DeclaredType(Proven::byPosition(m_original.type)));
    if (function().at(converted).type == TypeTable::never) {
        finishArmAsUnreachable();
        return;
    }
    m_continuationPredecessors.append(m_builder.currentBlock());
    m_builder.endExpansion(m_expansion, converted);
}

void SiteExpansion::finishArmWithReturn(ValueID result)
{
    RELEASE_ASSERT(result);
    const Program& program = m_context.program;
    const Block& end = function().at(m_builder.currentBlock());
    Opcode opcode = function().at(result).opcode;
    // The casts of the terminator were resolved against the type of the original call.
    bool endsWithTailCall = (opcode == m_original.opcode || opcode == Opcode::CallDirect) && !end.values.isEmpty() && end.values.last() == result
        && !end.handler && program.typeOf(function(), result).isSubsetOf(program.at(m_original.type), program);
    // An arm marks its call before it is known whether the call can stay a tail call.
    if (endsWithTailCall) {
        function().edit(result, ValueEdit::Definition).isTailCall = false;
        m_context.analyses.didChangeValues();
    } else
        result = convertTo(m_builder, result, DeclaredType(Proven::byPosition(m_original.type)));
    if (function().at(result).type == TypeTable::never) {
        finishArmAsUnreachable();
        return;
    }

    BlockID block = m_builder.currentBlock();
    Origin origin = m_builder.origin();
    Terminator terminator = *m_tailCall;
    terminator.setCallOperand(result);
    m_builder.setOrigin(terminator.origin());
    m_builder.emitTerminator(WTF::move(terminator));
    if (!endsWithTailCall) {
        m_builder.demoteReturnCall(block);
        m_context.build.statistics().count(StatisticID::ReturnCallsDemoted);
    }
    m_builder.setOrigin(origin);
}

void SiteExpansion::finishArmWithoutResult()
{
    RELEASE_ASSERT(isOpen() && m_expansion.results.isEmpty());
    m_continuationPredecessors.append(m_builder.currentBlock());
    m_builder.endExpansion(m_expansion, std::span<const ValueID> { });
}

void SiteExpansion::finishArmAsUnreachable()
{
    RELEASE_ASSERT(isOpen());
    m_builder.emitUnreachable();
}

void emitSplit(SiteExpansion& expansion, const ValueSplit& split, NOESCAPE const EmitSplitArm& emitArm, NOESCAPE const EmitSplitRest& emitRest)
{
    IRBuilder& builder = expansion.builder();
    const Program& program = expansion.context().program;

    ValueID current = split.value;
    for (uint32_t index = 0; index < split.arms.size(); ++index) {
        const SplitArm& arm = split.arms[index];
        bool entersWithoutTest = !split.hasRest && index + 1 == split.arms.size();
        if (entersWithoutTest) {
            RELEASE_ASSERT(program.typeOf(expansion.function(), current).isSubsetOf(program.at(arm.testedType), program));
            // The arm is entered because every test in front of it has failed.
            if (index && expansion.context().build.validates())
                builder.append(makeValidateType(current, arm.testedType));
            emitArm(index, current);
            RELEASE_ASSERT(!builder.currentBlock());
            return;
        }

        Expected<BlockFrequency> frequencyOfRemainingPaths = split.hasRest ? split.restFrequency : BlockFrequency::Rare;
        for (const SplitArm& remaining : split.arms.subspan(index + 1))
            frequencyOfRemainingPaths = moreFrequentOf(frequencyOfRemainingPaths, remaining.frequency);
        ValueID test = builder.appendTypeTest(current, arm.testedType);
        IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(test, OperandIndex::TypeTest::value, arm.frequency, frequencyOfRemainingPaths);
        emitArm(index, branch.provenOnTaken);
        RELEASE_ASSERT(!builder.currentBlock());
        builder.moveToEndOf(branch.notTaken);
        current = branch.provenOnNotTaken;
    }

    emitRest(current);
    RELEASE_ASSERT(!builder.currentBlock());
}

static PropertyAccessPlan planNamedAccess(const FunctionPassContext& context, const PropertyAccessQuery& query)
{
    return planPropertyAccess({ context.program, &context.inputs.flow }, query);
}

static PropertyAccessPlan planWithReceivers(const FunctionPassContext& context, std::optional<PropertyAccessQuery> query, MemberStart start, ReceiverCandidates&& receivers)
{
    if (!query)
        return { };
    query->start = start;
    query->receivers = WTF::move(receivers);
    return planNamedAccess(context, *query);
}

static PropertyAccessPlan planOnLayouts(const FunctionPassContext& context, const Value& access, const UnionPart& part)
{
    LayoutSetID stamp = PayloadWord::receivers(access.opcode) ? access.receivers() : LayoutSetID();
    ReceiverCandidates receivers = candidatesOf(context.program, part.type, nullptr, stamp);
    return planWithReceivers(context, namedAccessQueryOf(context, access), MemberStart::Instance, WTF::move(receivers));
}

static const Type* findExpectedType(const Program& program, const Function& function, ValueID id)
{
    TypeID expected = function.at(id).expected().predicted();
    return expected ? &program.at(expected) : nullptr;
}

static bool isAccessArmForm(AccessForm form)
{
    switch (form) {
    case AccessForm::Slot:
    case AccessForm::Constant:
    case AccessForm::Variable:
    case AccessForm::Namespace:
    case AccessForm::Builtin:
    case AccessForm::GetterInSlot:
    case AccessForm::SetterInSlot:
        return true;
    case AccessForm::ByName:
    case AccessForm::Method:
    case AccessForm::Call:
    case AccessForm::Split:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isResolvableForm(ElementAccessForm form)
{
    switch (form) {
    case ElementAccessForm::Array:
    case ElementAccessForm::TypedArray:
    case ElementAccessForm::String:
    case ElementAccessForm::ArgumentsElement:
        return true;
    case ElementAccessForm::ByKey:
    case ElementAccessForm::NamedKey:
    case ElementAccessForm::Split:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool canResolveElementAccess(const Program& program, const Function& function, const Value& access, const UnionPart& part)
{
    switch (part.kind) {
    case UnionPartKind::String:
    case UnionPartKind::BuiltinClass: {
        ValueID key = access.propertyOperand();
        return isResolvableForm(planElementAccess(program, {
            .access = *elementAccessOf(access.opcode),
            .writtenIn = program.sourceFunctionOf(function, access),
            .receiver = part.type,
            .key = program.typeOf(function, key),
            .keyExpected = findExpectedType(program, function, key),
            .stored = OperandPosition::value(access.opcode) ? &program.typeOf(function, access.valueOperand()) : nullptr,
        }).form);
    }
    case UnionPartKind::OtherObjects:
    case UnionPartKind::Nullish:
    case UnionPartKind::Boolean:
    case UnionPartKind::Number:
    case UnionPartKind::Symbol:
    case UnionPartKind::BigInt:
    case UnionPartKind::Absent:
    case UnionPartKind::Layouts:
    case UnionPartKind::Functions:
    case UnionPartKind::Builtins:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool resolvesOnPart(const FunctionPassContext& context, const Value& access, const UnionPart& part)
{
    const Program& program = context.program;
    if (!isNamedAccess(access.opcode))
        return canResolveElementAccess(program, context.function, access, part);

    switch (part.kind) {
    case UnionPartKind::Layouts: {
        PropertyAccessPlan plan = planOnLayouts(context, access, part);
        return plan.isResolved() || plan.form == AccessForm::Split;
    }
    case UnionPartKind::Functions: {
        LayoutID classLayout = classOfClassObject(program, part.type);
        if (!classLayout)
            return false;
        return planWithReceivers(context, namedAccessQueryOf(context, access), MemberStart::ClassObject, { .proof = ReceiverProof::Type, .level = KnowledgeLevel::Proven, .proven = { classLayout } }).isResolved();
    }
    case UnionPartKind::Boolean:
    case UnionPartKind::Number:
    case UnionPartKind::String:
    case UnionPartKind::Symbol:
    case UnionPartKind::BigInt:
    case UnionPartKind::BuiltinClass:
    case UnionPartKind::Builtins: {
        BuiltinUseInputs inputs { program, context.function, context.types, context.inputs.flow, context.analyses.effectContext(), &context.analyses, { } };
        return resolutionWithReceiver(inputs, access, part.type) == BuiltinUseResolution::Resolves;
    }
    case UnionPartKind::OtherObjects:
    case UnionPartKind::Nullish:
    case UnionPartKind::Absent:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The getter or the setter in a slot is called behind a test of the slot, and nobody places a test in code that runs once: there the arm would read by name as the rest does.
static bool waitsForTestThatNobodyPlaces(FunctionPassContext& context, BlockID block, const Value& access, const UnionPart& part)
{
    if (part.kind != UnionPartKind::Layouts || !isNamedAccess(access.opcode) || heatClassOf(context, block).predicted() != HeatClass::RunsOnce)
        return false;
    AccessForm form = planOnLayouts(context, access, part).form;
    return form == AccessForm::GetterInSlot || form == AccessForm::SetterInSlot;
}

static std::optional<uint32_t> splitOperandOf(const Value& value)
{
    if (!accessesByNameOrKey.contains(value.opcode))
        return std::nullopt;
    if (isNamedAccess(value.opcode))
        return *receiverOf(value.opcode);
    if (elementAccessOf(value.opcode))
        return OperandPosition::object(value.opcode);
    return std::nullopt;
}

AccessesResolvedOnParts countAccessesResolvedOnParts(FunctionPassContext& context, std::span<const ValueID> receivers, const UnionParts& parts)
{
    AccessesResolvedOnParts result;
    result.countOfPart.fill(0, parts.size());
    const BlockFrequencies& frequencies = context.analyses.blockFrequencies();
    for (ValueID receiver : receivers) {
        for (const Use& use : context.analyses.useLists().usesOf(receiver)) {
            if (use.kind != Use::Kind::Operand || frequencies.classOf(use.block).predicted() == BlockFrequency::Rare)
                continue;
            const Value& access = context.function.at(use.user);
            if (splitOperandOf(access) != use.operandIndex)
                continue;
            ++result.numberOfAccesses;
            bool isResolvable = false;
            for (size_t index = 0; index < parts.size(); ++index) {
                if (!resolvesOnPart(context, access, parts[index]))
                    continue;
                ++result.countOfPart[index];
                isResolvable = true;
            }
            result.numberOfResolvableAccesses += isResolvable;
            result.numberOfResolvableElementAccesses += isResolvable && !isNamedAccess(access.opcode);
        }
    }
    return result;
}

static Type testedTypeOf(const UnionPart& part)
{
    // A cast takes contents and storage kind from a tested type that names contents, so the bare class keeps what the value's own part says.
    return part.kind == UnionPartKind::BuiltinClass ? Type::builtinClass(part.type.classes[0].builtinClass).withoutClassProofs() : part.type;
}

static bool canSeparateByTest(const Program& program, TypeArena& types, const Type& whole, const UnionParts& parts, const UnionPart& part)
{
    // The object of such a built-in is no constant of the realm, so a test for it never passes.
    bool hasUntestableBuiltin = std::ranges::any_of(part.type.builtins, [&](BuiltinID builtin) {
        return !isTestableBuiltin(program.builtins(), builtin);
    });
    if (hasUntestableBuiltin)
        return false;
    if (part.kind == UnionPartKind::BuiltinClass && program.builtins().at(part.type.classes[0].builtinClass).instanceTest == BuiltinInstanceTest::None) {
        return std::ranges::none_of(parts, [&](const UnionPart& other) {
            return &other != &part && other.type.hasObjects();
        });
    }
    // Where another part passes the test too (a subclass of the tested class), the test narrows nothing, and the arm would be split again.
    return unionPartsOf(whole.castTo(testedTypeOf(part), types)).size() == 1;
}

namespace {

enum class LoopSplit : uint8_t { NotSplit, Split, OverLimit };

struct PlannedSplit {
    ValueSite site;
    uint32_t operandIndex { 0 };
    ValueSplit split;
    Vector<LayoutSetID, 4> armReceivers;
    LayoutSetID restReceivers;
    // The site is a call, and these are the read of its callee and the casts of what was read. Each arm reads the callee from its own receiver.
    Vector<ValueSite, 2> memberRead;
};

} // anonymous namespace

static Expected<bool> resolvesEveryAccess(FunctionPassContext& context, ValueID receiver, const BitVector* isInLoop, const UnionPart& part)
{
    const BlockFrequencies& frequencies = context.analyses.blockFrequencies();
    for (BlockID block : context.analyses.cfg().reversePostOrder()) {
        if ((isInLoop && !isInLoop->get(block.index())) || frequencies.classOf(block).predicted() == BlockFrequency::Rare)
            continue;
        for (ValueID id : context.function.at(block).values) {
            const Value& access = context.function.at(id);
            std::optional<uint32_t> operandIndex = splitOperandOf(access);
            if (operandIndex && context.analyses.identityOf(access.operand(*operandIndex)) == receiver && !resolvesOnPart(context, access, part))
                return false;
        }
    }
    return true;
}

// An operator takes it that gives nothing but a number, or that has a number on its other side.
static Expected<bool> isUsedAsNumber(FunctionPassContext& context, ValueID element)
{
    const Function& function = context.function;
    for (const Use& use : context.analyses.useLists().usesOf(element)) {
        if (use.kind != Use::Kind::Operand)
            continue;
        const Value& user = function.at(use.user);
        if (isCastThatProvesNothing(user.opcode) && isUsedAsNumber(context, use.user).predicted())
            return true;
        if (familyOf(user.opcode) != OperationFamily::Operator)
            continue;
        if (context.program.typeOf(function, use.user).hasOnlyPrimitives(Primitive::Number | Primitive::BigInt))
            return true;
        if (user.numberOfOperands() == 2 && context.program.expectedOrTypeOf(function, user.operand(!use.operandIndex)).predicted().isNumber())
            return true;
    }
    return false;
}

static Expected<bool> holdsNumbersByUse(FunctionPassContext& context, ValueID receiver, const BitVector& isInLoop)
{
    const Function& function = context.function;
    for (size_t block : isInLoop) {
        for (ValueID id : function.blocks[block].values) {
            const Value& access = function.at(id);
            if (!elementAccessOf(access.opcode) || context.analyses.identityOf(access.objectOperand()) != receiver)
                continue;
            if (OperandPosition::value(access.opcode) ? context.program.expectedOrTypeOf(function, access.valueOperand()).predicted().isNumber() : isUsedAsNumber(context, id).predicted())
                return true;
        }
    }
    return false;
}

static Expected<bool> isExpectedToHoldNumbers(FunctionPassContext& context, ValueID receiver, ValueID narrowestName, const BitVector& isInLoop)
{
    TypeID expected = context.function.at(narrowestName).expected().predicted();
    if (!expected)
        return holdsNumbersByUse(context, receiver, isInLoop);
    const Type& type = context.types.at(expected);
    if (type.anyObject || type.anyFunction || type.layouts || !type.functions.isEmpty() || !type.builtins.isEmpty())
        return false;
    if (type.classes.size() != 1 || type.classes[0].builtinClass != BuiltinClasses::Array)
        return false;
    const Type& contents = context.types.at(type.classes[0].expectedContentsAt(0).predicted());
    if (contents.isNumberOrNullish())
        return true;
    return contents.primitives & Primitive::Number && holdsNumbersByUse(context, receiver, isInLoop).predicted();
}

// What a test in front of the loop has proven of the object holds inside it.
static ValueID findNarrowestName(FunctionPassContext& context, ValueID receiver, ProgramPoint front)
{
    const Program& program = context.program;
    ValueID narrowest;
    for (uint32_t index = 0; index < context.function.values.size(); ++index) {
        ValueID name(index);
        if (context.analyses.identityOf(name) != receiver || !context.analyses.dominates(name, front))
            continue;
        if (!narrowest || program.typeOf(context.function, name).isSubsetOf(program.typeOf(context.function, narrowest), program))
            narrowest = name;
    }
    return narrowest;
}

static TypeID chooseTestedType(FunctionPassContext& context, ValueID receiver, ValueID narrowestName, const BitVector& isInLoop, BlockID front, bool& restTakesEveryValue)
{
    const Program& program = context.program;
    const Type& whole = program.typeOf(context.function, narrowestName);
    if (isClosedUnion(whole)) {
        UnionParts parts = unionPartsOf(whole);
        const UnionPart* last = nullptr;
        unsigned numberOfServingParts = 0;
        for (const UnionPart& part : parts) {
            if (!canSeparateByTest(program, context.types, whole, parts, part) || !resolvesEveryAccess(context, receiver, &isInLoop, part).predicted())
                continue;
            ++numberOfServingParts;
            last = &part;
        }
        if (numberOfServingParts < 2)
            return { };
        return context.types.intern(testedTypeOf(*last));
    }

    // Nobody could follow the object. What is asked only for what a view of numbers has may be one as well as the array that it is taken for: bytes, which the host hands out, or a kind that the program constructs.
    if (!whole.anyObject || !context.build.recordedOptions().usesExpectedTypes() || !isExpectedToHoldNumbers(context, receiver, narrowestName, isInLoop).predicted())
        return { };
    const BuiltinTable& builtins = program.builtins();
    Vector<BuiltinClassID, 4> kinds { BuiltinClasses::Uint8Array };
    for (uint32_t index = 0; index < builtins.classCount(); ++index) {
        const BuiltinClass& view = builtins.at(BuiltinClassID(index));
        if (hasInlineElementAccess(builtins.typedArrayTypeOf(view.id)) && builtins.typedArrayClassOf(view.id) == view.id && !kinds.contains(view.id) && context.inputs.flow.usesOf(view.constructor).contains(BuiltinUse::Constructed))
            kinds.append(view.id);
    }
    // Where `new` is given a callee that nobody knows every kind counts as constructed, which says nothing.
    if (kinds.size() > context.limit(LimitID::MaxDispatchCases))
        kinds.shrink(1);
    for (BuiltinClassID kind : kinds) {
        UnionParts parts = unionPartsOf(Type::builtinClass(kind).withoutClassProofs());
        TypeID testedType = context.types.intern(parts[0].type);
        Value test = IRBuilder::make(Opcode::TypeTest, { narrowestName });
        test.setTestedType(testedType);
        if (context.analyses.findEarlierTest(test, front) == EarlierTest::Failed || !resolvesEveryAccess(context, receiver, nullptr, parts[0]).predicted())
            continue;
        restTakesEveryValue = true;
        return testedType;
    }
    return { };
}

static LoopSplit splitLoop(FunctionPassContext& context, const NaturalLoop& loop, BitVector& isFinishedHeader, unsigned numberOfSplitLoops)
{
    const Program& program = context.program;
    Function& function = context.function;
    FunctionAnalyses& analyses = context.analyses;
    BlockID header = loop.header();
    if (analyses.blockFrequencies().classOf(header).predicted() != BlockFrequency::Normal || heatClassOf(context, header).predicted() != HeatClass::Hot)
        return LoopSplit::NotSplit;
    bool isLoopOfProgram = std::ranges::any_of(function.at(header).values, [&](ValueID value) {
        return function.at(value).opcode == Opcode::LoopHint;
    });
    if (!isLoopOfProgram)
        return LoopSplit::NotSplit;
    std::optional<Edge> entry = analyses.naturalLoops().findSingleEntry(loop);
    if (!entry)
        return LoopSplit::NotSplit;

    Vector<BlockID> region;
    BitVector isInLoop(function.blocks.size());
    uint64_t bytes = 0;
    for (unsigned index = 0; index < loop.size(); ++index) {
        BlockID block = loop.at(index);
        isInLoop.set(block.index());
        region.append(block);
        bytes += estimatedCodeBytes(program, function, function.at(block));
    }
    if (context.exceeds(LimitID::MaxBytesOfSeparatedLoop, bytes))
        return LoopSplit::NotSplit;

    const UseLists& uses = analyses.useLists();
    const BlockFrequencies& frequencies = analyses.blockFrequencies();
    Vector<ValueID, 4> receivers;
    for (BlockID block : region) {
        if (frequencies.classOf(block).predicted() == BlockFrequency::Rare)
            continue;
        for (ValueID id : function.at(block).values) {
            const Value& access = function.at(id);
            if (!elementAccessOf(access.opcode))
                continue;
            ValueID receiver = analyses.identityOf(access.objectOperand());
            if (!isInLoop.get(uses.blockOf(receiver).index()) && !receivers.contains(receiver))
                receivers.append(receiver);
        }
    }
    ValueID receiver;
    ValueID narrowestName;
    TypeID testedType;
    bool restTakesEveryValue = false;
    for (ValueID candidate : receivers) {
        narrowestName = findNarrowestName(context, candidate, pointBeforeTerminatorOf(function, entry->from));
        // The test in front of the loop also runs where no turn gets to an access.
        if (!narrowestName || context.program.typeOf(function, narrowestName).mayBeUnspecified())
            continue;
        testedType = chooseTestedType(context, candidate, narrowestName, isInLoop, entry->from, restTakesEveryValue);
        if (!testedType)
            continue;
        receiver = candidate;
        break;
    }
    if (!receiver || RegionCloner::checkCanDuplicate(analyses, region.span()) != CloneCheck::CanClone)
        return LoopSplit::NotSplit;
    if (context.exceeds(LimitID::MaxSeparatedLoopsPerFunction, numberOfSplitLoops + 1))
        return LoopSplit::OverLimit;

    // Whatever else names the object in front of the loop is the same object inside it.
    Vector<ValueID, 4> names;
    Vector<Use, 16> usesInLoop;
    for (uint32_t index = 0; index < function.values.size(); ++index) {
        ValueID name(index);
        if (analyses.identityOf(name) != receiver || uses.locationOf(name).placement == ValuePlacement::Detached || isInLoop.get(uses.blockOf(name).index()))
            continue;
        size_t numberOfUsesBefore = usesInLoop.size();
        for (const Use& use : uses.usesOf(name)) {
            if (isInLoop.get(use.block.index()))
                usesInLoop.append(use);
        }
        if (usesInLoop.size() > numberOfUsesBefore)
            names.append(name);
    }

    IRBuilder builder(analyses.context(), function, context.types, &analyses);
    builder.setOrigin(function.at(entry->from).terminator.origin());
    BlockID front = builder.splitEdge(*entry);
    Expected<BlockFrequency> frequency = function.at(front).frequency;
    Arguments arguments(function.at(front).terminator.next().arguments);
    function.edit(front, { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments }).terminator = Terminator { };
    builder.moveToEndOf(front);
    if (restTakesEveryValue)
        builder.emitFailureOfPredictions(Target { header, Arguments(arguments), { } });
    ValueID test = builder.appendTypeTest(narrowestName, testedType);
    IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(test, OperandIndex::TypeTest::value, frequency, frequency);

    auto narrows = [&](ValueID proven, ValueID name) {
        return program.typeOf(function, proven).isSubsetOf(program.typeOf(function, name), program);
    };
    RegionCloner cloner(builder, function);
    for (ValueID name : names) {
        if (narrows(branch.provenOnTaken, name))
            cloner.mapValue(name, branch.provenOnTaken);
    }
    cloner.cloneBlocks(region.span(), [](ValueID, Value&) { });
    builder.moveToEndOf(branch.taken);
    builder.emitJump(cloner.cloneOf(header), Arguments(arguments));
    builder.moveToEndOf(branch.notTaken);
    builder.emitJump(header, WTF::move(arguments));
    if (!restTakesEveryValue) {
        for (const Use& use : usesInLoop) {
            if (narrows(branch.provenOnNotTaken, usedValueOf(function, use)))
                replaceUse(function, use, branch.provenOnNotTaken);
        }
    }
    analyses.didChangeControlFlow();
    cloner.repairSSA(analyses);
    analyses.didChangeControlFlow();

    for (BlockID block : region) {
        isFinishedHeader.set(block.index());
        isFinishedHeader.set(cloner.cloneOf(block).index());
    }
    // The next kind of view gets the next copy.
    if (restTakesEveryValue)
        isFinishedHeader.clear(header.index());
    return LoopSplit::Split;
}

// A loop that takes the elements of one object, which may be of several classes, gets a copy for one class. The class is tested once, in front of the loop.
static bool splitLoops(FunctionPassContext& context)
{
    BitVector isFinishedHeader;
    unsigned numberOfSplitLoops = 0;
    for (;;) {
        const NaturalLoops& loops = context.analyses.naturalLoops();
        Vector<const NaturalLoop*, 8> candidates;
        for (unsigned index = 0; index < loops.numLoops(); ++index) {
            if (!isFinishedHeader.get(loops.loop(index).header().index()))
                candidates.append(&loops.loop(index));
        }
        // Outer loops come first: the test stands in front of the widest loop that does not change the object.
        AOT::stableSort(candidates, std::ranges::greater { }, &NaturalLoop::size);
        LoopSplit result = LoopSplit::NotSplit;
        for (size_t index = 0; index < candidates.size() && result == LoopSplit::NotSplit; ++index) {
            isFinishedHeader.set(candidates[index]->header().index());
            result = splitLoop(context, *candidates[index], isFinishedHeader, numberOfSplitLoops);
        }
        if (result != LoopSplit::Split)
            return numberOfSplitLoops;
        ++numberOfSplitLoops;
    }
}

// Behind a join of the arms nobody knows any more which method belongs to which receiver. `x.m()` is split as a whole where the read may move down to the call.
static void moveToMemberCall(FunctionPassContext& context, PlannedSplit& plan)
{
    const Function& function = context.function;
    const Value& read = function.at(plan.site.value);
    if (read.opcode != Opcode::PropGet || plan.split.value != read.objectOperand())
        return;

    const UseLists& useLists = context.analyses.useLists();
    std::optional<Use> use = useLists.singleUse(plan.site.value);
    while (use && use->kind == Use::Kind::Operand && isCast(function.at(use->user).opcode) && !use->operandIndex)
        use = useLists.singleUse(use->user);
    if (!use || use->kind != Use::Kind::Operand || function.at(use->user).opcode != Opcode::Call || use->operandIndex != OperandIndex::Call::callee)
        return;
    if (function.at(use->user).thisValueOperand() != plan.split.value)
        return;

    ValueLocation location = useLists.locationOf(use->user);
    ValueSite callSite { location.block, location.index, use->user };
    MemberRead memberRead = findMemberRead(context, callSite);
    if (memberRead.isEmpty() || !memberRead.canSinkToCall || memberRead.read().value != plan.site.value)
        return;

    plan.site = callSite;
    plan.operandIndex = OperandIndex::Call::thisValue;
    plan.memberRead = WTF::move(memberRead.links);
}

static std::optional<PlannedSplit> planSplitByParts(FunctionPassContext& context, const ValueSite& site, uint32_t operandIndex)
{
    struct ChosenPart {
        const UnionPart* part;
        bool isLikely;
        unsigned numberOfResolvedAccesses;
    };
    const Program& program = context.program;
    const Function& function = context.function;
    const Value& value = function.at(site.value);
    ValueID receiver = value.operand(operandIndex);
    const Type& whole = program.typeOf(function, receiver);
    UnionParts parts = unionPartsOf(whole);
    if (parts.size() < 2)
        return std::nullopt;

    const Type& expected = program.expectedOrTypeOf(function, receiver).predicted();
    AccessesResolvedOnParts evidence = countAccessesResolvedOnParts(context, singleElementSpan(receiver), parts);
    Vector<ChosenPart, 4> chosen;
    bool restResolvesOnLikelyPart = false;
    for (size_t index = 0; index < parts.size(); ++index) {
        const UnionPart& part = parts[index];
        if (!resolvesOnPart(context, value, part) || waitsForTestThatNobodyPlaces(context, site.block, value, part))
            continue;
        // A class without an instance test overlaps every class, so a class is expected where the expected type names it.
        bool isLikely = part.kind == UnionPartKind::BuiltinClass ? !!expected.findClassPart(part.type.classes[0].builtinClass) : part.type.overlaps(expected, program);
        if (!canSeparateByTest(program, context.types, whole, parts, part)) {
            restResolvesOnLikelyPart |= isLikely;
            continue;
        }
        chosen.append(ChosenPart { &part, isLikely, evidence.countOfPart[index] });
    }
    // What is expected is tested first, then the part that serves most of what the function does with the value.
    bool isAccessedByNumber = evidence.numberOfResolvableElementAccesses;
    AOT::stableSort(chosen, [&](const ChosenPart& a, const ChosenPart& b) {
        if (a.isLikely != b.isLikely)
            return a.isLikely;
        if (a.numberOfResolvedAccesses != b.numberOfResolvedAccesses)
            return a.numberOfResolvedAccesses > b.numberOfResolvedAccesses;
        return isAccessedByNumber && a.part->type.hasObjects() && !b.part->type.hasObjects();
    });
    if (context.exceeds(LimitID::MaxDispatchCases, chosen.size(), site.value)) {
        size_t limit = static_cast<size_t>(context.limit(LimitID::MaxDispatchCases));
        restResolvesOnLikelyPart |= limit < chosen.size() && chosen[limit].isLikely;
        chosen.shrink(limit);
    }
    if (chosen.isEmpty())
        return std::nullopt;

    Expected<BlockFrequency> frequency = function.at(site.block).frequency;
    Expected<BlockFrequency> unlikelyFrequency = rarerOf(frequency, BlockFrequency::Unlikely);
    bool hasCompleteStamp = PayloadWord::receivers(value.opcode) && value.receivers() && program.at(value.receivers()).listsEveryLayout();

    // What is expected goes somewhere: where no arm takes it, the rest does.
    bool restIsLikely = restResolvesOnLikelyPart || !chosen.first().isLikely;
    PlannedSplit plan { site, operandIndex, { receiver, { }, chosen.size() < parts.size(), restIsLikely ? frequency : unlikelyFrequency }, { }, { }, { } };
    for (const ChosenPart& arm : chosen) {
        plan.split.arms.append(SplitArm { context.types.intern(testedTypeOf(*arm.part)), arm.isLikely ? frequency : unlikelyFrequency });
        bool stampsLayouts = arm.part->kind == UnionPartKind::Layouts && !hasCompleteStamp;
        plan.armReceivers.append(stampsLayouts ? arm.part->type.layouts : LayoutSetID());
    }
    return plan;
}

static bool isOnlyComparedByIdentity(FunctionPassContext& context, ValueID read)
{
    const Function& function = context.function;
    const UseLists& useLists = context.analyses.useLists();
    std::optional<Use> use = useLists.singleUse(read);
    while (use && use->kind == Use::Kind::Operand && isCast(function.at(use->user).opcode) && !use->operandIndex)
        use = useLists.singleUse(use->user);
    return use && use->kind == Use::Kind::Operand && function.at(use->user).opcode == Opcode::StrictEq;
}

static std::optional<PlannedSplit> planSplitByArms(FunctionPassContext& context, const ValueSite& site, const PropertyAccessQuery& query, const PropertyAccessPlan& access)
{
    if (isCold(heatClassOf(context, site.block)).predicted())
        return std::nullopt;

    const Value& value = context.function.at(site.value);
    const MemberPartition& partition = access.partition;
    PropertyAccessQuery queryOfArm = query;
    bool isOnlyAskedOrCalled = value.opcode == Opcode::PropGet && isNobodysObject(context.analyses, site.value);
    queryOfArm.isOnlyCalled = query.isOnlyCalled || isOnlyAskedOrCalled;
    DispatchPlan dispatch = planDispatch(context, partition, query.receivers, site.value, [&](const MemberArm& arm) {
        PropertyAccessPlan plan = planWithReceivers(context, queryOfArm, query.start, { .proof = query.receivers.proof, .level = query.receivers.level, .proven = arm.layouts });
        // `response.emit === ownEmit`: behind the tests of its class the method is a constant, and the comparison folds.
        bool comparesMethod = plan.form == AccessForm::Method && !plan.capturesReceiver && (isOnlyAskedOrCalled || isOnlyComparedByIdentity(context, site.value));
        return isAccessArmForm(plan.form) || comparesMethod;
    });
    if (dispatch.arms.isEmpty())
        return std::nullopt;
    // Arms for the objects that lack a member alone, where the read stays as it is for those that have it: whoever reads a name expects an object that has it.
    bool restFindsMember = isSymbolOfProgram(context.program.at(query.key).kind) && std::ranges::any_of(dispatch.rest, [&](LayoutID layout) {
        const ClassMember* member = findMemberOfClass(context.program, layout, ClassSide::Prototype, query.key);
        return member && member->mutability != MemberMutability::Fixed;
    });
    if (restFindsMember)
        return std::nullopt;

    PlannedSplit plan { .site = site, .operandIndex = *receiverOf(value.opcode) };
    plan.split = createSplit(context, dispatch, partition, value.operand(plan.operandIndex), context.function.at(site.block).frequency);
    for (uint32_t armIndex : dispatch.arms)
        plan.armReceivers.append(context.types.completeLayoutSet(partition.arms[armIndex].layouts.span()));
    if (dispatch.hasRest())
        plan.restReceivers = context.types.completeLayoutSet(dispatch.rest.span());
    return plan;
}

static PlannedSplit planSplitOfKey(FunctionPassContext& context, const ValueSite& site, std::span<const Type> keys, bool hasRest)
{
    const Value& value = context.function.at(site.value);
    Expected<BlockFrequency> frequency = context.function.at(site.block).frequency;
    PlannedSplit plan { site, *OperandPosition::property(value.opcode), { value.propertyOperand(), { }, hasRest, rarerOf(frequency, BlockFrequency::Unlikely) }, { }, { }, { } };
    for (const Type& key : keys)
        plan.split.arms.append(SplitArm { context.types.intern(key), frequency });
    plan.armReceivers.fill(LayoutSetID(), keys.size());
    return plan;
}

// `const { kState } = require(..)` may find nothing as far as anybody knows, where somebody may see the module before its body has ended: the key is the symbol or undefined.
// With the symbol the access is an access by name, whatever is known about the receiver: few layouts have a field under a symbol, so the name says what the receiver is expected to be.
static std::optional<PlannedSplit> planSplitOfSymbolKey(FunctionPassContext& context, const ValueSite& site)
{
    const Type& keyType = context.program.typeOf(context.function, context.function.at(site.value).propertyOperand());
    if (!keyType.mayBeUndefinedOrNull() || !keyType.hasOnlyPrimitives(Primitive::LiteralSymbol | Primitive::UndefinedOrNull) || keyType.symbolLiterals.size() != 1)
        return std::nullopt;
    Type symbol = Type::symbolLiteral(keyType.symbolLiterals[0]);
    if (!constantNameOf(context.program, context.program.sourceFunctionOf(context.function, context.function.at(site.value)), symbol))
        return std::nullopt;
    return planSplitOfKey(context, site, singleElementSpan(symbol), true);
}

// Read under a number, an arguments object that stays in its function is never allocated.
static bool needsCellForReadsByKeyOnly(FunctionAnalyses& analyses, ValueID arguments)
{
    const EscapeAnalysis& escape = analyses.escape();
    if (escape.gaveUp())
        return false;
    std::optional<ObjectID> object = escape.singleObject(arguments);
    if (!object || escape.at(*object).state == EscapeState::Unknown)
        return false;
    if (analyses.function().at(escape.at(*object).definition).opcode != Opcode::CreateUnmappedArguments)
        return false;
    return std::ranges::all_of(escape.usesOf(*object), [&](const ObjectUse& objectUse) {
        const Use& use = objectUse.use;
        return !objectUse.isEscape || (use.kind == Use::Kind::Operand && analyses.function().at(use.user).opcode == Opcode::ElemGet && use.operandIndex == OperandIndex::ElemGet::object);
    });
}

// With a number the access reads or writes an element. What else the key may be names a property, and that is rare: the key of an element is undefined where it was read past the end.
static std::optional<PlannedSplit> planSplitOfNumberKey(FunctionPassContext& context, const ValueSite& site)
{
    const Program& program = context.program;
    const Function& function = context.function;
    const Value& value = function.at(site.value);
    ValueID key = value.propertyOperand();
    ValueID receiver = value.objectOperand();
    UnionParts parts = unionPartsOf(program.typeOf(function, key));
    auto number = std::ranges::find(parts, UnionPartKind::Number, &UnionPart::kind);
    if (parts.size() < 2 || number == parts.end())
        return std::nullopt;
    // A key that may be anything is tested for a number only where that saves an arguments object.
    const Type& receiverType = program.typeOf(function, receiver);
    const ClassPart* receiverClass = receiverType.singleBuiltinClass();
    if (!isClosedUnion(parts) && (!receiverClass || receiverClass->builtinClass != BuiltinClasses::Arguments || receiverType.mayBeUndefinedOrNull()))
        return std::nullopt;

    ValueID stored = OperandPosition::value(value.opcode) ? value.valueOperand() : ValueID();
    ElementAccessForm form = planElementAccess(program, {
        .access = *elementAccessOf(value.opcode),
        .writtenIn = program.sourceFunctionOf(function, value),
        .receiver = receiverType,
        .receiverExpected = findExpectedType(program, function, receiver),
        .key = number->type,
        .stored = stored == key ? &number->type : stored ? &program.typeOf(function, stored) : nullptr,
        .storedExpected = stored && stored != key ? findExpectedType(program, function, stored) : nullptr,
    }).form;
    if (!isResolvableForm(form))
        return std::nullopt;
    if (form == ElementAccessForm::ArgumentsElement && (value.opcode != Opcode::ElemGet || !needsCellForReadsByKeyOnly(context.analyses, receiver)))
        return std::nullopt;
    return planSplitOfKey(context, site, singleElementSpan(number->type), true);
}

static std::optional<PlannedSplit> planSplitByKeys(FunctionPassContext& context, const ValueSite& site)
{
    const Value& value = context.function.at(site.value);
    const Type& keyType = context.program.typeOf(context.function, value.propertyOperand());
    if (keyType.primitives & Primitive::Number)
        return planSplitOfNumberKey(context, site);
    bool mayBeNullish = keyType.mayBeUndefinedOrNull();
    if (!keyType.hasOnlyPrimitives(Primitive::LiteralString | Primitive::UndefinedOrNull) || keyType.stringLiterals.size() + mayBeNullish < 2)
        return std::nullopt;

    Vector<Type, 4> keys;
    for (StringID text : keyType.stringLiterals) {
        keys.append(Type::stringLiteral(text));
        ConstantKey constantKey = constantKeyOf(context.program, keys.last());
        if (!constantKey)
            return std::nullopt;
        Value named = value;
        becomeNamedAccess(named, internKey(context.types, constantKey));
        std::optional<PropertyAccessQuery> query = namedAccessQueryOf(context, named);
        if (!query || !planNamedAccess(context, *query).isResolved())
            return std::nullopt;
    }
    if (context.exceeds(LimitID::MaxDispatchCases, keys.size(), site.value))
        return std::nullopt;
    return planSplitOfKey(context, site, keys.span(), mayBeNullish);
}

static std::optional<PlannedSplit> planSplit(FunctionPassContext& context, const ValueSite& site)
{
    const Function& function = context.function;
    const Value& value = function.at(site.value);
    std::optional<uint32_t> operandIndex = splitOperandOf(value);
    if (!operandIndex)
        return std::nullopt;
    if (!isNamedAccess(value.opcode) && !isRecordedAsRare(function, site.block).predicted()) {
        if (std::optional<PlannedSplit> plan = planSplitOfSymbolKey(context, site))
            return plan;
    }
    // Nothing is proven about the objects of such a type, and testing every unknown value for its primitive parts would only grow the code.
    if (context.program.typeOf(function, value.operand(*operandIndex)).anyObject)
        return std::nullopt;
    if (isRecordedAsRare(function, site.block).predicted())
        return std::nullopt;
    auto isInRareBlock = [&] {
        return context.analyses.blockFrequencies().classOf(site.block).predicted() == BlockFrequency::Rare;
    };

    if (!isNamedAccess(value.opcode)) {
        std::optional<ElementAccessPlan> plan = planElementAccessOf(context.program, function, value);
        if (!plan || (plan->form != ElementAccessForm::ByKey && plan->form != ElementAccessForm::Split) || isInRareBlock())
            return std::nullopt;
        if (plan->form == ElementAccessForm::ByKey)
            return planSplitByKeys(context, site);
        return planSplitByParts(context, site, *operandIndex);
    }

    std::optional<PropertyAccessQuery> query = namedAccessQueryOf(context, value);
    if (!query)
        return std::nullopt;
    const ReceiverCandidates& receivers = query->receivers;
    if (!receivers.hasOtherParts && receivers.proven.size() < 2)
        return std::nullopt;
    if (isInRareBlock())
        return std::nullopt;

    PropertyAccessPlan access = planNamedAccess(context, *query);
    switch (access.form) {
    case AccessForm::Split:
        if (receivers.hasOtherParts)
            return planSplitByParts(context, site, *operandIndex);
        return planSplitByArms(context, site, *query, access);
    case AccessForm::ByName:
        if (receivers.isProven())
            return std::nullopt;
        return planSplitByParts(context, site, *operandIndex);
    case AccessForm::Slot:
    case AccessForm::Constant:
    case AccessForm::Variable:
    case AccessForm::Namespace:
    case AccessForm::Builtin:
    case AccessForm::Method:
    case AccessForm::Call:
    case AccessForm::GetterInSlot:
    case AccessForm::SetterInSlot:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static PassOutcome splitAccesses(FunctionPassContext& context)
{
    Vector<PlannedSplit> plans;
    forEachValueSite(context.analyses, context.function, accessesByNameOrKey, [&](const ValueSite& site) {
        if (std::optional<PlannedSplit> plan = planSplit(context, site)) {
            moveToMemberCall(context, *plan);
            plans.append(WTF::move(*plan));
        }
    });
    if (plans.isEmpty())
        return PassOutcome::Unchanged;

    const CFG& cfg = context.analyses.cfg();
    AOT::stableSort(plans, { }, [&](const PlannedSplit& plan) {
        return std::pair { cfg.reversePostOrderIndex(plan.site.block), plan.site.index };
    });

    IRBuilder builder(context.analyses.context(), context.function, context.types, &context.analyses);
    SiteExpansion expansion(context, builder);
    for (const PlannedSplit& plan : plans | std::views::reverse) {
        RELEASE_ASSERT(plan.split.hasRest || plan.split.arms.size() > 1);
        expansion.begin(plan.site);
        if (!plan.memberRead.isEmpty()) {
            Vector<Value, 2> copies;
            for (const ValueSite& link : plan.memberRead) {
                copies.append(context.function.at(link.value));
                becomeUndefined(context.function, link.value);
            }
            expansion.addReplaced(copies.first());
            expansion.setOperationsBeforeOriginal(WTF::move(copies), OperandIndex::Call::callee);
            context.analyses.didChangeValues();
        }
        emitSplit(expansion, plan.split,
            [&](uint32_t armIndex, ValueID narrowed) {
                expansion.finishArm(expansion.appendOriginal({ plan.operandIndex, narrowed, plan.armReceivers[armIndex], { }, true }));
            },
            [&](ValueID narrowed) {
                expansion.finishArm(expansion.appendOriginal({ plan.operandIndex, narrowed, plan.restReceivers, { }, true }));
            });
        expansion.end();
    }

    context.build.statistics().count(StatisticID::UnionsSplit, plans.size());
    // Each rewrite tells the analyses of its edits as it makes them.
    context.keptAnalyses = allAnalyses;
    return PassOutcome::Changed;
}

PassOutcome splitUnions(FunctionPassContext& context)
{
    bool didSplitLoops = context.analyses.naturalLoops().numLoops() && splitLoops(context);
    if (didSplitLoops) {
        retypeValues(context);
        resolveBuiltins(context);
        resolveAccesses(context);
    }
    PassOutcome outcome = splitAccesses(context);
    if (!didSplitLoops)
        return outcome;
    retypeValues(context);
    context.keptAnalyses = { };
    return PassOutcome::Changed;
}

static_assert(OperandIndex::IsSlotFilled::object == testedOperandIndex);
static_assert(OperandIndex::HasNoExtraProperties::object == testedOperandIndex);
static_assert(OperandIndex::HasNoFieldOutsideSlot::object == testedOperandIndex);
static_assert(OperandIndex::HasBirthFieldsFilled::object == testedOperandIndex);
static_assert(OperandIndex::IsNotFrozen::object == testedOperandIndex);
static_assert(OperandIndex::HasLayoutPrototype::object == testedOperandIndex);
static_assert(OperandIndex::BelongsToInstance::object == testedOperandIndex);

static constexpr std::array objectProofsInTestOrder {
    Proof::OwnInstance,
    Proof::PrototypeLinkUnchanged,
    Proof::NoExtraProperties,
    Proof::NoFieldOutsideSlot,
    Proof::BirthFieldsFilled,
    Proof::NotFrozen,
    Proof::SlotFilled,
};

static constexpr ProofSet testableProofs = [] {
    ProofSet proofs { Proof::NotNullish };
    for (Proof proof : objectProofsInTestOrder)
        proofs.add(proof);
    return proofs;
}();

void setSlotStamp(Value& value, const SlotStamp& stamp)
{
    value.setKey(stamp.key);
    value.setReceivers(stamp.receivers);
    value.setLevel(stamp.level);
    if (stamp.accessSite)
        value.setAccessSite(stamp.accessSite);
}

Value makeObjectTest(Proof proof, ValueID object, const SlotStamp& stamp)
{
    switch (proof) {
    case Proof::SlotFilled: {
        Value test = IRBuilder::make(Opcode::IsSlotFilled, { object });
        // Sites share a test when its payload is equal, so the test does not name the access site it was made for.
        setSlotStamp(test, { stamp.key, stamp.receivers, stamp.level, { } });
        return test;
    }
    case Proof::NoExtraProperties:
        return IRBuilder::make(Opcode::HasNoExtraProperties, { object });
    case Proof::NoFieldOutsideSlot:
        return IRBuilder::make(Opcode::HasNoFieldOutsideSlot, { object });
    case Proof::BirthFieldsFilled:
        return IRBuilder::make(Opcode::HasBirthFieldsFilled, { object });
    case Proof::NotFrozen:
        return IRBuilder::make(Opcode::IsNotFrozen, { object });
    case Proof::OwnInstance:
        return IRBuilder::make(Opcode::BelongsToInstance, { object });
    case Proof::PrototypeLinkUnchanged:
        return IRBuilder::make(Opcode::HasLayoutPrototype, { object });
    case Proof::None:
    case Proof::TestedType:
    case Proof::Truthiness:
    case Proof::Equality:
    case Proof::IntegerBound:
    case Proof::ExactFunction:
    case Proof::CaseValue:
    case Proof::NotNullish:
    case Proof::Shape:
    case Proof::Pristine:
    case Proof::ArrayStorage:
    case Proof::InBounds:
    case Proof::NotThenable:
    case Proof::MemberUnchanged:
    case Proof::EvaluatedOnce:
    case Proof::KeyOrder:
    case Proof::UnmodifiedGenerator:
    case Proof::EnumeratorCurrent:
    case Proof::SourceOpen:
    case Proof::TextMatches:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static MemberConditionKind kindInRecordOf(MemberCondition::Kind kind)
{
    switch (kind) {
    case MemberCondition::Kind::MemberUnchanged:
        return MemberConditionKind::MemberUnchanged;
    case MemberCondition::Kind::PrototypeLinkUnchanged:
        return MemberConditionKind::PrototypeLinkUnchanged;
    case MemberCondition::Kind::ClassEvaluatedOnce:
        return MemberConditionKind::ClassEvaluatedOnce;
    case MemberCondition::Kind::InstancePrototypeUnchanged:
        return MemberConditionKind::InstancePrototypeUnchanged;
    case MemberCondition::Kind::MemberNotHidden:
        return MemberConditionKind::MemberNotHidden;
    case MemberCondition::Kind::MemberPassedByStores:
        return MemberConditionKind::MemberPassedByStores;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static MemberConditionRecord recordOf(const MemberCondition& condition)
{
    return { .kind = kindInRecordOf(condition.kind), .layout = condition.layout, .side = condition.side, .key = condition.key };
}

// Placement gives each set of conditions one bit, which the run time clears with the bit of any of them. Sites share a test when they ask for the same set, so the set is listed in one order.
Value makeConditionsTest(TypeArena&, std::span<const MemberCondition> conditions)
{
    RELEASE_ASSERT(!conditions.empty() && std::ranges::is_sorted(conditions));
    Vector<MemberConditionRecord, 4> records(conditions.size(), [&](size_t index) {
        return recordOf(conditions[index]);
    });

    Value test = IRBuilder::make(Opcode::AreMembersUnchanged);
    test.setConditions(records.span());
    return test;
}

bool testsCoverConditions(std::span<const Value> tests, std::span<const MemberCondition> conditions)
{
    return std::ranges::all_of(conditions, [&](const MemberCondition& condition) {
        MemberConditionRecord record = recordOf(condition);
        return std::ranges::any_of(tests, [&](const Value& test) {
            return test.opcode == Opcode::AreMembersUnchanged && std::ranges::contains(test.conditions(), record);
        });
    });
}

static Type testableTypeOf(const PropertyAccessContext& context, const Type& stored, const UnionPart& part)
{
    switch (part.kind) {
    case UnionPartKind::Layouts:
        // A value whose type lists layouts counts as an object of this Instance, which a test of its layout does not show.
        if (stored.layouts || instanceCountOf(context) == InstanceCount::AtMostOne)
            return part.type;
        return Type::never();
    case UnionPartKind::BuiltinClass:
        return part.type.withoutClassProofs();
    case UnionPartKind::OtherObjects:
    case UnionPartKind::Nullish:
    case UnionPartKind::Boolean:
    case UnionPartKind::Number:
    case UnionPartKind::String:
    case UnionPartKind::Symbol:
    case UnionPartKind::BigInt:
    case UnionPartKind::Absent:
    case UnionPartKind::Functions:
    case UnionPartKind::Builtins:
        return part.type;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// A TypeTest proves no contents, storage or pristine state: a class part counts only when the value's own type has the proofs the field asks for.
Type testableStoredTypeOf(const PropertyAccessContext& context, const Type& stored, const Type& fieldType, TypeArena& types)
{
    Type testable = Type::never();
    for (const UnionPart& part : unionPartsOf(fieldType.intersectWith(stored, types))) {
        Type testablePart = testableTypeOf(context, stored, part);
        if (stored.castTo(testablePart, types).isSubsetOf(fieldType, context.program))
            testable = testable.unionWith(testablePart, types);
    }
    return testable;
}

AccessTests createAccessTests(FunctionPassContext& context, const PropertyAccessPlan& plan, const AccessTestInputs& inputs)
{
    RELEASE_ASSERT((plan.requiredProofs | testableProofs) == testableProofs);

    const Program& program = context.program;
    TypeArena& types = context.types;
    AccessTests result;

    switch (plan.receiverTest) {
    case ReceiverTest::None:
        // The layouts of the receiver are proven, and undefined and null are members of its type beside them.
        if (plan.requiredProofs.contains(Proof::NotNullish))
            result.tests.append(makeTypeTest(inputs.receiver, TypeTable::object));
        break;
    case ReceiverTest::Type:
        result.receiverLayoutTest = 0;
        result.tests.append(makeTypeTest(inputs.receiver, types.intern(Type::forLayouts(plan.receivers.span(), types))));
        break;
    case ReceiverTest::ClassObject:
        result.receiverLayoutTest = 0;
        result.tests.append(makeTypeTest(inputs.receiver, types.intern(Type::forFunction(plan.testedClassObject))));
        break;
    }

    for (Proof proof : objectProofsInTestOrder) {
        if (!plan.requiredProofs.contains(proof))
            continue;
        Value test = makeObjectTest(proof, inputs.receiver, inputs.stamp);
        if (proof == Proof::SlotFilled && plan.testsDeclaredType)
            test.setHoldsDeclaredType(true);
        result.tests.append(WTF::move(test));
    }

    // The first test that reads the receiver also finds out that it is undefined or null: it faults. So nothing that branches stands between the check for those two and that test.
    if (!plan.conditions.isEmpty())
        result.tests.append(makeConditionsTest(types, plan.conditions.span()));

    // The test of the stored value comes last: whoever plans the store behind the tests takes it off the end.
    if (plan.stores() && !plan.storedValueFits) {
        Type fieldType = storedTypeOf(program, plan.receivers.span(), inputs.stamp.key, types);
        Type testable = testableStoredTypeOf({ program, &context.inputs.flow }, program.typeOf(context.function, inputs.stored), fieldType, types);
        if (testable.isNever())
            return { };
        result.tests.append(makeTypeTest(inputs.stored, types.intern(testable)));
    }
    return result;
}

std::optional<ReceiverCandidates> receiversEstablishedBy(const Program& program, std::span<const Value> tests)
{
    // The test of a receiver comes first. A later TypeTest is about the stored value, whose type may have layouts too.
    if (tests.empty() || tests[0].opcode != Opcode::TypeTest)
        return std::nullopt;
    const Type& tested = program.at(tests[0].testedType());
    if (LayoutID classLayout = classOfClassObject(program, tested))
        return ReceiverCandidates::underGuard(singleElementSpan(classLayout));
    if (!tested.layouts)
        return std::nullopt;
    return ReceiverCandidates::underGuard(membersOf(program, tested));
}

DispatchPlan planDispatch(FunctionPassContext& context, const MemberPartition& partition, const ReceiverCandidates& candidates, ValueID site, NOESCAPE const CanEmitArm& canEmitArm)
{
    DispatchPlan plan;
    // First the arms that find the member, then those with more expected members, then those with more members.
    Vector<std::tuple<bool, uint32_t, uint32_t, LayoutID>, 4> ranks;
    for (uint32_t index = 0; index < partition.arms.size(); ++index) {
        const MemberArm& arm = partition.arms[index];
        LayoutList expectedMembers = arm.layouts;
        expectedMembers.intersect(candidates.expected.predicted());
        ranks.append({ arm.answer.kind == MemberKind::Absent, ~static_cast<uint32_t>(expectedMembers.size()), ~static_cast<uint32_t>(arm.layouts.size()), arm.layouts[0] });
        if (canEmitArm(arm))
            plan.arms.append(index);
    }
    AOT::sort(plan.arms, { }, [&](uint32_t arm) {
        return ranks[arm];
    });
    if (context.exceeds(LimitID::MaxDispatchCases, plan.arms.size(), site))
        plan.arms.shrink(static_cast<size_t>(context.limit(LimitID::MaxDispatchCases)));

    plan.rest = partition.rest;
    for (uint32_t index = 0; index < partition.arms.size(); ++index) {
        if (!plan.arms.contains(index))
            plan.rest.unite(partition.arms[index].layouts);
    }
    plan.restIsLikely = plan.rest.overlaps(candidates.expected.predicted());
    return plan;
}

ValueSplit createSplit(FunctionPassContext& context, const DispatchPlan& dispatch, const MemberPartition& partition, ValueID receiver, Expected<BlockFrequency> siteFrequency)
{
    const Program& program = context.program;
    TypeArena& types = context.types;
    Expected<BlockFrequency> unlikelyFrequency = rarerOf(siteFrequency, BlockFrequency::Unlikely);

    LayoutList expected(membersOf(program, program.expectedOrTypeOf(context.function, receiver).predicted()));
    auto hasExpectedMember = [&](uint32_t armIndex) {
        return partition.arms[armIndex].layouts.overlaps(expected);
    };
    bool hasExpectation = dispatch.restIsLikely || std::ranges::any_of(dispatch.arms, hasExpectedMember);

    ValueSplit split;
    split.value = receiver;
    LayoutList covered;
    for (uint32_t armIndex : dispatch.arms) {
        const LayoutList& layouts = partition.arms[armIndex].layouts;
        bool isLikely = split.arms.isEmpty() || !hasExpectation || hasExpectedMember(armIndex);
        split.arms.append(SplitArm { types.intern(Type::forLayouts(layouts.span(), types)), isLikely ? siteFrequency : unlikelyFrequency });
        covered.unite(layouts);
    }
    split.restFrequency = dispatch.restIsLikely ? siteFrequency : unlikelyFrequency;

    const Type& receiverType = program.typeOf(context.function, receiver);
    split.hasRest = !receiverType.isSubsetOf(Type::forLayouts(covered.span(), types), program);
    if (!split.hasRest || split.arms.isEmpty())
        return split;

    // A family keeps the layouts that nothing allocates. No object has one of them, so they ask for no arm and for no rest: the arm that is entered without a test takes them along.
    LayoutList neverAllocated;
    for (LayoutID member : membersOf(program, receiverType)) {
        if (program.at(member).flow.allocation == AllocationState::NeverAllocated)
            neverAllocated.add(member);
    }
    covered.unite(neverAllocated);
    if (neverAllocated.isEmpty() || !receiverType.isSubsetOf(Type::forLayouts(covered.span(), types), program))
        return split;

    LayoutList layoutsOfLastArm = partition.arms[dispatch.arms.last()].layouts;
    layoutsOfLastArm.unite(neverAllocated);
    split.arms.last().testedType = types.intern(Type::forLayouts(layoutsOfLastArm.span(), types));
    split.hasRest = false;
    return split;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
