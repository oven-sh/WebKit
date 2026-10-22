/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTResolveCalls.h"

#if ENABLE(AOT)

#include "AOTBlockFrequencies.h"
#include "AOTBuildContext.h"
#include "AOTFlowApply.h"
#include "AOTFunctionAnalyses.h"
#include "AOTResultRules.h"
#include "AOTStatistics.h"
#include "AOTValueIdentity.h"
#include "AOTValueSubstitution.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static ValueID findLiteralList(const Function& function, const UseLists& useLists, const Value& construct)
{
    ValueID id = construct.operand(OperandIndex::SuperConstructList::list);
    while (useLists.singleUse(id) && function.at(id).opcode == Opcode::Upcast)
        id = function.at(id).operand(OperandIndex::Upcast::value);

    const Value& list = function.at(id);
    if (!useLists.singleUse(id) || list.opcode != Opcode::NewArray || !list.holes().empty() || list.capacityOperand())
        return { };
    return id;
}

static Vector<ValueID, 8> collectConstructArguments(const Function& function, const Value& construct)
{
    if (construct.opcode != Opcode::SuperConstructList)
        return Vector<ValueID, 8>(construct.variadicOperands());

    const Value& list = function.at(definitionThroughUpcasts(function, construct.operand(OperandIndex::SuperConstructList::list)));
    RELEASE_ASSERT(list.opcode == Opcode::NewArray);
    return Vector<ValueID, 8>(list.variadicOperands());
}

static bool allocatesOwnThis(const Function& constructor)
{
    if (constructor.has(FunctionFlags::AllocatesOwnThis))
        return true;

    switch (constructor.kind) {
    case FunctionKind::BaseConstructor:
    case FunctionKind::DerivedConstructor:
        return !constructor.has(FunctionFlags::HasThis);
    case FunctionKind::Plain:
    case FunctionKind::Arrow:
    case FunctionKind::Method:
    case FunctionKind::Getter:
    case FunctionKind::Setter:
    case FunctionKind::Initializer:
    case FunctionKind::Module:
    case FunctionKind::CommonJsModule:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The run time reads the prototype property at the first allocation of a site. No code can change the property of a class.
// An assigned prototype object is stored before every read of the property, and a construction is one.
static bool hasOnePrototypeObject(const Program& program, LayoutID layout)
{
    const Layout& constructed = program.at(layout);
    if (!isEvaluatedOnce(program, constructed))
        return false;
    const ClassFlow& classFlow = constructed.classFlow;
    return !constructed.has(LayoutFlags::IsConstructorFunction) || classFlow.prototypeProperty == MemberMutability::Fixed || classFlow.hasAssignedPrototype;
}

static DirectCallRequest createConstructionRequest(FunctionID constructor, ValueID newTarget, std::span<const ValueID> arguments)
{
    DirectCallRequest request;
    request.callee = constructor;
    request.newTarget = newTarget;
    request.arguments = arguments;
    request.isConstruction = true;
    return request;
}

// Without a new.target to read the prototype from, the object has the prototype object of its class.
static ValueID emitThis(const Program& program, LayoutID allocatedClass, ValueID newTargetToRead, NOESCAPE const InsertValue& emit)
{
    ValueID prototype;
    if (newTargetToRead) {
        Value read = IRBuilder::make(Opcode::GetConstructPrototype, { newTargetToRead });
        read.setLayout(declaredLayoutOf(program, allocatedClass));
        prototype = emit(WTF::move(read));
    }

    Value object = IRBuilder::make(Opcode::NewObject, { prototype });
    object.setLayout(allocatedClass);
    const Vector<AllocationClassID>& allocationClasses = program.at(allocatedClass).flow.allocationClasses;
    if (allocationClasses.size() == 1)
        object.setAllocationClass(allocationClasses.first());
    return emit(WTF::move(object));
}

static ValueID appendThis(IRBuilder& builder, LayoutID allocatedClass, ValueID newTargetToRead)
{
    auto append = [&](auto&& value) {
        return builder.append(WTF::move(value));
    };
    return emitThis(builder.context().program, allocatedClass, newTargetToRead, InsertValue(append));
}

ValueID appendThisOfConstructorFunction(IRBuilder& builder, FunctionID constructor, ValueID closure)
{
    const Program& program = builder.context().program;
    LayoutID constructed = program.at(constructor).constructedClass;
    return appendThis(builder, constructed, hasOnePrototypeObject(program, declaredLayoutOf(program, constructed)) ? ValueID() : closure);
}

static bool isClassConstructor(const Function& function)
{
    return function.kind == FunctionKind::BaseConstructor || function.kind == FunctionKind::DerivedConstructor;
}

// new.target selects what is allocated, and the allocation is handed its prototype object alone. That object names a class, whose prototype property nobody can store to.
// What a function has as its prototype names nothing, so a function may be new.target here only where its own layout is allocated.
static bool isAlwaysClass(const Program& program, const Type& newTarget)
{
    if (!newTarget.isObject() || !newTarget.hasOnlyKnownCallees() || newTarget.layouts || !newTarget.builtins.isEmpty())
        return false;
    return std::ranges::all_of(newTarget.functions, [&](FunctionID function) {
        return isClassConstructor(program.at(function));
    });
}

// The run time exchanges the object for an instance of the class that new.target selects, so the class that holds the super call stands for all of them.
// A class that nobody constructs itself has no placement: the one class below it that has instances is allocated in its place, and with several the run time chooses.
static LayoutID findClassAllocatedBySuperCall(const Program& program, LayoutID holder)
{
    // The source names a built-in class as the parent. Its constructor makes the instances, so the layout has no placement either, whatever flow finds above the class object.
    if (builtinBaseClassOf(program, holder))
        return { };
    if (program.at(holder).flow.allocation != AllocationState::NeverAllocated)
        return holder;
    std::span<const LayoutID> allocatedBelow = program.layoutsBelow(holder);
    return allocatedBelow.size() == 1 ? allocatedBelow[0] : LayoutID();
}

static uint32_t constructorOperandIndexOf(Opcode construct)
{
    return construct == Opcode::Construct ? *OperandPosition::callee(construct) : *OperandPosition::parent(construct);
}

bool isConstructorFunction(const Program& program, const Function& function)
{
    if (function.kind != FunctionKind::Plain || function.constructorForm() != ConstructorForm::Base)
        return false;
    if (!function.has(FunctionFlags::HasThis) || !function.constructedClass)
        return false;
    return program.at(declaredLayoutOf(program, function.constructedClass)).has(LayoutFlags::IsConstructorFunction);
}

// A tail call in a constructor is none: what it returns decides whether the object is returned in its place. A test that leaves for generic code has to stand where that code was made.
static bool canTestWhatIsReturned(const Function& function)
{
    for (const Block& block : function.blocks) {
        if (block.terminator.kind != TerminatorKind::ReturnCall)
            continue;
        for (TailCast cast : block.terminator.casts()) {
            if (cast.resolution == CastResolution::Unresolved || cast.resolution == CastResolution::Check)
                return false;
        }
    }
    return true;
}

static bool admitsLayout(const Program& program, TypeID id, LayoutID layout)
{
    const Type& type = program.at(id);
    return type.anyObject || std::ranges::binary_search(membersOf(program, type), layout);
}

bool canHaveConstructBody(const Program& program, FunctionID id)
{
    const Function& function = program.at(id);
    if (!function.hasBody() || !function.mayBeCloned() || function.results.size() != 1 || !isConstructorFunction(program, function))
        return false;
    bool boundsThis = function.thisType.type && !function.thisType.flags.contains(PositionFlags::IsExpected);
    if (boundsThis && !admitsLayout(program, function.thisType.type, function.constructedClass))
        return false;
    return canTestWhatIsReturned(function);
}

static FunctionID closureFunctionOf(const Program& program, FunctionID function)
{
    while (FunctionID source = program.at(function).derivation.source)
        function = source;
    return function;
}

static bool readsPrototypeOfNewTarget(const Program& program, const Function& constructor)
{
    return !hasOnePrototypeObject(program, declaredLayoutOf(program, constructor.constructedClass));
}

static void becomeConstructBody(const Program& program, Function& body, const Function& original, FunctionID source)
{
    unsigned thisParameter = *original.thisParameterIndex();
    Vector<ParameterOrigin> parameterMap;
    for (unsigned index = 0; index < original.numberOfEntryParameters(); ++index) {
        if (index != thisParameter)
            parameterMap.append(ParameterOrigin { index });
    }

    body.role = FunctionRole::ConstructBody;
    body.creator = CreatorRecord::Version { .provenVersion = source };
    body.flags.remove(FunctionFlags::HasThis);
    body.flags.add(FunctionFlags::AllocatesOwnThis);
    body.thisType = DeclaredType { .type = TypeTable::anything };
    if (readsPrototypeOfNewTarget(program, original) && !original.has(FunctionFlags::HasNewTarget)) {
        body.flags.add(FunctionFlags::HasNewTarget);
        parameterMap.insert(*body.newTargetParameterIndex(), ParameterOrigin { });
    }
    body.capabilities = initialCapabilitiesOf(body);
    body.derivation = Derivation { .source = source, .parameterMap = WTF::move(parameterMap) };
}

void emitReturnOfConstruction(IRBuilder& builder, ValueID returned, ValueID object)
{
    const Function& function = builder.function();
    const Type* returnedType = returned ? &builder.context().program.typeOf(function, returned) : nullptr;
    if (returnedType && returnedType->isNever()) {
        builder.emitUnreachable();
        return;
    }
    if (!returnedType || !returnedType->hasObjects()) {
        builder.emitReturn(object);
        return;
    }
    if (returnedType->isObject()) {
        builder.emitReturn(returned);
        return;
    }

    ValueID isObject = builder.appendTypeTest(returned, Type::object());
    IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(isObject, 0, BlockFrequency::Unlikely, BlockFrequency::Normal);
    builder.emitReturn(branch.provenOnTaken);
    builder.moveToEndOf(branch.notTaken);
    builder.emitReturn(object);
}

static void declareWhatIsReturned(Function& body, TypeArena& types, ValueID object)
{
    TypeID returned = TypeTable::never;
    for (const Block& block : body.blocks) {
        if (block.terminator.kind == TerminatorKind::Return)
            returned = types.unionOf(returned, body.at(block.terminator.valueOperand()).type);
    }
    body.results[0] = DeclaredType { .type = returned == TypeTable::never ? body.at(object).type : returned };
}

static void returnWhatIsConstructed(IRBuilder& builder, ValueID object)
{
    Function& body = builder.function();
    size_t numberOfBlocks = body.blocks.size();
    for (size_t index = 0; index < numberOfBlocks; ++index) {
        BlockID block(index);
        if (body.at(block).terminator.kind == TerminatorKind::ReturnCall)
            builder.demoteReturnCall(block);
        if (body.at(block).terminator.kind != TerminatorKind::Return)
            continue;

        ValueID returned = body.at(block).terminator.valueOperand();
        builder.setOrigin(body.at(block).terminator.origin());
        body.edit(block).terminator = Terminator();
        builder.moveToEndOf(block);
        emitReturnOfConstruction(builder, returned, object);
    }
}

Function buildConstructBody(const DerivedFunctionKey& key, DerivedFunctionInputs& inputs)
{
    const Program& program = inputs.program;
    const Function& original = program.at(key.target);
    RELEASE_ASSERT(canHaveConstructBody(program, key.target));
    Function body = original;
    becomeConstructBody(program, body, original, key.target);

    IRBuilder builder(AnalysisContext { inputs.build, program, &inputs.flow, nullptr, key.target }, body, inputs.types);
    builder.setOrigin({ original.position, 0 });
    if (body.at(entryBlock).handler) {
        builder.splitBlock(ProgramPoint::atHeadOf(entryBlock));
        builder.removeHandler(entryBlock);
    }

    unsigned thisIndex = *original.thisParameterIndex();
    ValueID thisParameter = body.at(entryBlock).parameters[thisIndex];
    ValueID newTarget;
    if (std::optional<unsigned> index = original.newTargetParameterIndex())
        newTarget = body.at(entryBlock).parameters[*index];
    else if (body.has(FunctionFlags::HasNewTarget))
        newTarget = builder.addParameter(entryBlock, TypeTable::anything);

    auto insertAtHead = [&](auto&& value) {
        return builder.insertBefore(ProgramPoint::atHeadOf(entryBlock), WTF::move(value));
    };

    // new.target is a closure of the function here, which is what says that its prototype property belongs to the layout.
    std::optional<unsigned> calleeIndex = original.calleeParameterIndex();
    ValueID closure = calleeIndex ? body.at(entryBlock).parameters[*calleeIndex] : newTarget;
    ValueID validation;
    ValueID ownClosure;
    if (closure) {
        TypeID closureType = inputs.types.intern(Type::forFunction(closureFunctionOf(program, key.target)));
        if (inputs.build.validates())
            validation = insertAtHead(makeValidateType(closure, closureType));
        ownClosure = insertAtHead(makeAssumeType(closure, closureType));
    }
    ValueID object = emitThis(program, original.constructedClass, readsPrototypeOfNewTarget(program, original) ? ownClosure : ValueID(), InsertValue(insertAtHead));
    builder.applyInsertions();

    ValueSubstitution substitution(body);
    substitution.replace(thisParameter, object);
    if (ownClosure) {
        substitution.replace(closure, ownClosure);
        if (newTarget && newTarget != closure)
            substitution.replace(newTarget, ownClosure);
    }
    substitution.apply(body, nullptr);
    if (validation)
        body.edit(validation).setValueOperand(closure);
    if (ownClosure)
        body.edit(ownClosure).setValueOperand(closure);

    BitVector removedParameters(body.at(entryBlock).parameters.size());
    removedParameters.set(thisIndex);
    builder.removeParameters(entryBlock, removedParameters);
    if (!original.has(FunctionFlags::HasNewTarget) && newTarget) {
        auto& parameters = body.edit(entryBlock).parameters;
        RELEASE_ASSERT(parameters.last() == newTarget);
        parameters.removeLast();
        parameters.insert(*body.newTargetParameterIndex(), newTarget);
    }

    returnWhatIsConstructed(builder, object);
    declareWhatIsReturned(body, inputs.types, object);
    return body;
}

PlanOrDecline<ConstructArm> CallResolver::planConstructArm(const CallSite& site, const CalleeCandidates& candidates, FunctionID member)
{
    const Value& construct = m_function.at(site.position.value);
    bool isSuperCall = site.opcode != Opcode::Construct;
    ValueID callee = construct.operand(constructorOperandIndexOf(site.opcode));

    ConstructArm arm;
    arm.call = createCalleeArm(candidates, member);

    ValueID newTarget = construct.newTargetOperand();
    bool isOwnNewTarget = !isSuperCall && (!newTarget || areSameObject(newTarget, callee));
    if (isOwnNewTarget && canHaveConstructBody(m_program, member)) {
        arm.call.callee = m_context.derivedFunction(derivedFunctionKey(FunctionRole::ConstructBody, member));
        return arm;
    }

    LayoutID constructed = m_program.at(member).constructedClass;
    if (!allocatesOwnThis(m_program.at(member))) {
        if (!(isSuperCall ? construct.layout() : constructed))
            return std::unexpected(CallDeclineReason::CalleeNotCallable);
        if (!isOwnNewTarget && !(isSuperCall && isAlwaysClass(m_program, typeOf(newTarget))))
            return std::unexpected(CallDeclineReason::None);
        arm.allocatedClass = isSuperCall ? findClassAllocatedBySuperCall(m_program, construct.layout()) : constructed;
        if (!arm.allocatedClass)
            return std::unexpected(CallDeclineReason::None);
    } else if (isOwnNewTarget && canTakeAllocatedThis(m_program, member)) {
        arm.allocatedClass = layoutAllocatedBy(m_program, member);
        arm.call.callee = m_context.derivedFunction(derivedFunctionKey(FunctionRole::AllocatedThisVersion, member));
    }
    arm.readsConstructPrototype = arm.allocatedClass && !(isOwnNewTarget && hasOnePrototypeObject(m_program, declaredLayoutOf(m_program, arm.allocatedClass)));
    return arm;
}

PlanOrDecline<ConstructPlan> CallResolver::planConstruct(const CallSite& site)
{
    const Value& construct = m_function.at(site.position.value);
    if (site.opcode == Opcode::SuperConstructList && !findLiteralList(m_function, m_context.analyses.useLists(), construct))
        return std::unexpected(CallDeclineReason::None);

    CalleeCandidates candidates = calleeCandidatesOf(m_program, m_function, construct, construct.operand(constructorOperandIndexOf(site.opcode)));
    PlanOrDecline<Vector<FunctionID, 4>> members = chooseDispatchedCallees(site, candidates, [&](FunctionID member) {
        return canConstructDirectly(m_program, member);
    });
    if (!members)
        return std::unexpected(members.error());

    // A constructor without an arm is constructed by the original operation, with whatever else the value may be.
    ConstructPlan plan;
    Vector<FunctionID, 4> armConstructors;
    CallDeclineReason decline = CallDeclineReason::None;
    for (FunctionID member : *members) {
        PlanOrDecline<ConstructArm> arm = planConstructArm(site, candidates, member);
        if (!arm) {
            decline = arm.error();
            continue;
        }
        plan.arms.append(WTF::move(*arm));
        armConstructors.append(member);
    }
    if (plan.arms.isEmpty())
        return std::unexpected(decline);

    plan.callee = candidates.value;
    plan.proof = candidates.proof;
    plan.split = createCalleeSplit(site, candidates, armConstructors.span());
    return plan;
}

void CallResolver::emitConstructArm(const ConstructArm& arm, ValueID closure, std::span<const ValueID> arguments)
{
    const Value& original = m_expansion.original();
    ValueID newTarget = original.newTargetOperand() ? original.newTargetOperand() : closure;
    ValueID thisValue = arm.allocatedClass ? appendThis(m_builder, arm.allocatedClass, arm.readsConstructPrototype ? newTarget : ValueID()) : ValueID();
    Vector<ValueID, 8> captures = capturesOf(arm.call, closure, { });

    DirectCallRequest request = createConstructionRequest(arm.call.callee, newTarget, arguments);
    request.captures = captures.span();
    request.thisValue = thisValue;
    if (m_program.at(arm.call.callee).has(FunctionFlags::TakesCallee))
        request.calleeValue = closure;
    request.accessSite = original.accessSite();
    ValueID returned = appendDirectCall(m_builder, request);

    if (arm.allocatedClass)
        finishConstruction(returned, thisValue);
    else
        m_expansion.finishArm(returned);
}

void CallResolver::rewriteConstruct(const CallSite& site, const ConstructPlan& plan)
{
    m_expansion.begin(site.position);
    Vector<ValueID, 8> arguments = collectConstructArguments(m_function, m_expansion.original());
    if (plan.proof == CalleeProof::Type)
        appendCalleeValidation(m_builder, plan.callee);
    emitSplit(m_expansion, plan.split,
        [&](uint32_t armIndex, ValueID narrowed) {
            emitConstructArm(plan.arms[armIndex], narrowed, arguments.span());
        },
        [&](ValueID narrowed) {
            m_expansion.finishArm(m_expansion.appendOriginal({ constructorOperandIndexOf(site.opcode), narrowed }));
        });
    m_expansion.end();
    m_context.build.statistics().count(StatisticID::ConstructsResolved);
}

void CallResolver::finishConstruction(ValueID returned, ValueID thisValue)
{
    const Type& returnedType = typeOf(returned);
    if (returnedType.isNever()) {
        m_expansion.finishArmAsUnreachable();
        return;
    }
    if (!returnedType.hasObjects()) {
        m_expansion.finishArm(thisValue);
        return;
    }

    BlockFrequency frequency = m_expansion.frequency();
    ValueID isObject = m_builder.appendTypeTest(returned, Type::object());
    IRBuilder::ProvenBranch branch = m_builder.emitBranchWithProof(isObject, 0, rarerOf(frequency, BlockFrequency::Unlikely), frequency);
    m_expansion.finishArm(branch.provenOnTaken);
    m_builder.moveToEndOf(branch.notTaken);
    m_expansion.finishArm(thisValue);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
