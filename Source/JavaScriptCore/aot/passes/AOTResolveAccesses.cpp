/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTResolveAccesses.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsOfAccesses.h"
#include "AOTEffectsOfRules.h"
#include "AOTFlowResults.h"
#include "AOTFunctionAnalyses.h"
#include "AOTNarrowFieldTypes.h"
#include "AOTPropertyAccessCache.h"
#include "AOTResultRules.h"
#include "AOTTypePropagation.h"
#include "AOTTypeTestSite.h"
#include "AOTValueIdentity.h"
#include "AOTValueSubstitution.h"
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet storesListedAtAllocations { Opcode::PropDefine, Opcode::PropSet };

static bool isCalleeOperand(Opcode user, uint32_t operandIndex)
{
    return (user == Opcode::Call && operandIndex == OperandIndex::Call::callee)
        || (user == Opcode::CallList && operandIndex == OperandIndex::CallList::callee);
}

static Effects effectsOfVariableAccess(const Program& program, const PropertyAccessPlan& plan)
{
    Value access(plan.operation);
    access.setVariable(plan.member.variable);
    Effects effects = declaredEffectsOf(program, access);
    if (plan.operation == Opcode::GetModuleVariable)
        effects.merge(declaredBoundOf(Opcode::CheckInitialized));
    return effects;
}

static AccessOperands operandsOfElementAccess(const Value& access)
{
    return { access.objectOperand(), OperandPosition::value(access.opcode) ? access.valueOperand() : ValueID(), { } };
}

static bool canMarkIntegralKey(const Value& access, const ElementAccessPlan& plan)
{
    return plan.keyIsIntegral && !plan.testsKeyIsNumber && FlagMask::keyIsIntegral(access.opcode) && !access.keyIsIntegral();
}

static ElementAccessPlan createIntegralKeyPlan()
{
    ElementAccessPlan byKey;
    byKey.keyIsIntegral = true;
    return byKey;
}

static std::optional<PropertyAccessQuery> namedAccessQueryOf(const FunctionPassContext& context, const Value& access)
{
    std::optional<PropertyAccessQuery> query = propertyAccessQueryOf(context.program, context.function, access);
    if (!query)
        return std::nullopt;
    ASSERT(accessesToResolve.contains(access.opcode));

    query->maxMembers = context.limit(LimitID::MaxAccessPlanMembers);
    return query;
}

// All that propertyAccessQueryOf() and planPropertyAccess() take from a named access and its function, but for who uses what is read.
static std::optional<SiteContent> contentOfNamedAccess(const FunctionPassContext& context, const Value& access)
{
    const Program& program = context.program;
    const Function& function = context.function;
    const FlowResults& flow = context.inputs.flow;
    Opcode opcode = access.opcode;
    if (!isNamedAccess(opcode) || (FlagMask::isFreeName(opcode) && access.isFreeName()))
        return std::nullopt;

    SiteContent content(static_cast<uint32_t>(RefusalKind::Access) << 24 | static_cast<uint32_t>(opcode) << 8 | access.flags);
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

static NamedAccessSite planNamedAccessSite(FunctionPassContext& context, const ValueSite& site, PropertyAccessQuery&& query)
{
    // Who uses what is read is asked, of the lists of all uses in the function, only for a plan that follows from it.
    PropertyAccessPlan plan = planPropertyAccess({ context.program, &context.inputs.flow }, query);
    if (query.planFollowsFromIsOnlyCalled && context.function.at(site.value).opcode == Opcode::PropGet && isOnlyCalled(context.analyses, site.value)) {
        query.isOnlyCalled = true;
        plan = planPropertyAccess({ context.program, &context.inputs.flow }, query);
    }
    return NamedAccessSite { site, WTF::move(query), WTF::move(plan) };
}

std::optional<NamedAccessSite> planNamedAccessSite(FunctionPassContext& context, const ValueSite& site)
{
    std::optional<PropertyAccessQuery> query = namedAccessQueryOf(context, context.function.at(site.value));
    if (!query)
        return std::nullopt;
    return planNamedAccessSite(context, site, WTF::move(*query));
}

std::optional<ElementAccessSite> planElementAccessSite(FunctionPassContext& context, const ValueSite& site)
{
    std::optional<ElementAccessPlan> plan = planElementAccessOf(context.program, context.function, context.function.at(site.value));
    if (!plan)
        return std::nullopt;
    ASSERT(accessesToResolve.contains(context.function.at(site.value).opcode));
    return ElementAccessSite { site, WTF::move(*plan) };
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
            if (!isCalleeOperand(user, use.operandIndex))
                return false;
            isCalled = true;
        }
    }
    return isCalled;
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

static Effects effectsOfCallingGetterInSlot(FunctionPassContext& context, const PropertyAccessPlan& plan, const SlotStamp& stamp)
{
    std::optional<Vector<FunctionID, 2>> getters = gettersInSlots(context.program, plan.receivers.span(), stamp.key);
    if (!getters)
        return Effects::unknownCode();
    Effects effects = effectsOfAccessForm(context.program, plan.operation, stamp.key, stamp.receivers);
    for (FunctionID getter : *getters)
        effects.merge(effectsOfCalling({ context.program, context.inputs.effects }, getter).normal);
    return effects;
}

Effects effectsAfterRewrite(FunctionPassContext& context, const PropertyAccessPlan& plan, const SlotStamp& stamp)
{
    switch (plan.form) {
    case AccessForm::Slot:
        return effectsOfAccessForm(context.program, plan.operation, stamp.key, stamp.receivers);
    case AccessForm::Constant:
        return Effects::none();
    case AccessForm::Builtin:
        return declaredBoundOf(Opcode::Builtin);
    case AccessForm::Method:
        return declaredBoundOf(Opcode::NewClosure);
    case AccessForm::Variable:
        return effectsOfVariableAccess(context.program, plan);
    case AccessForm::Namespace:
        return declaredBoundOf(Opcode::ModuleNamespace);
    case AccessForm::GetterInSlot:
        return effectsOfCallingGetterInSlot(context, plan, stamp);
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

AccessRewriter::AccessRewriter(FunctionPassContext& context, const InsertValue& insert)
    : m_context(context)
    , m_insert(insert)
    , m_checksEffects(context.build.options().verifiesProgram)
{
}

ValueID AccessRewriter::insert(Value&& value, Origin origin)
{
    value.setOrigin(origin);
    return m_insert(WTF::move(value));
}

void AccessRewriter::finish(const ValueSite& site, const ReplacedEffects& replaced)
{
    Value& value = valueAt(site);
    value.type = resultTypeOf(m_context.program, m_context.function, value, m_context.types);
    value.setExpected(expectedTypeOf(m_context.program, m_context.function, value, m_context.types));
    checkRewrite(m_context, site, replaced);
}

void AccessRewriter::rewrite(const NamedAccessSite& site, const AccessOperands& operands)
{
    switch (site.plan.form) {
    case AccessForm::Slot:
        becomeSlotOperation(site, operands);
        break;
    case AccessForm::Constant:
        becomeConstant(site);
        break;
    case AccessForm::Builtin:
        becomeBuiltin(site);
        break;
    case AccessForm::Method:
        becomeMethodClosure(site, operands);
        break;
    case AccessForm::Variable:
        becomeVariableAccess(site, operands);
        break;
    case AccessForm::Namespace:
        becomeNamespace(site);
        break;
    case AccessForm::GetterInSlot:
        becomeCallOfGetterInSlot(site, operands);
        break;
    case AccessForm::ByName:
    case AccessForm::Call:
    case AccessForm::Split:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

void AccessRewriter::rewrite(const ElementAccessSite& site, const AccessOperands& operands)
{
    switch (site.plan.form) {
    case ElementAccessForm::NamedKey:
        becomeNamedAccess(site);
        return;
    case ElementAccessForm::Array:
        becomeArrayAccess(site, operands);
        return;
    case ElementAccessForm::TypedArray:
        becomeTypedArrayAccess(site, operands);
        return;
    case ElementAccessForm::ArgumentsElement:
        // The read stays by key. Only an arguments object that is never built has another form for it.
        if (canMarkIntegralKey(valueAt(site.site), site.plan))
            valueAt(site.site).setKeyIsIntegral(true);
        return;
    case ElementAccessForm::ByKey:
    case ElementAccessForm::String:
    case ElementAccessForm::Split:
        valueAt(site.site).setKeyIsIntegral(true);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

AccessResolver::AccessResolver(FunctionPassContext& context)
    : m_context(context)
    , m_builder({ context.build, context.program, &context.inputs.flow, &context.inputs.effects, context.id }, context.function, context.types, &context.analyses)
{
}

// `{ __proto__: null, first: f(), second() { } }`, and `const made = { }; made.first = f(); made.second = g();`. From its allocation on an object is in nobody's hands but the function's,
// until the function hands it on: it is not frozen, its prototype is the one it was born with, and a name that has not been written lies nowhere in it. Nobody can tell when it was allocated either.
// So the allocation moves down behind the values of the first stores and lists them, in the order in which the stores would have created the properties, and the stores are gone.
// A value that the slot may not admit is left to the run time: the allocation lists something that fits in its place, as storeUnprovenInitialValues() does, and the definition comes behind the allocation.
// It ends at the first use that is none of these: a spread, a computed key, an accessor, a name for the second time, a value that the slot may not admit where nothing can stand in for it,
// a store below another prototype (which may find a setter), and whatever may hand the object on.
bool AccessResolver::listFirstStoresAtAllocation(BlockID block, uint32_t allocationIndex)
{
    Function& function = m_context.function;
    ValueID allocationID = function.at(block).values[allocationIndex];
    const Layout& layout = m_context.program.at(function.at(allocationID).layout());

    struct UseInBlock {
        uint32_t index;
        ValueID user;
        bool isReceiver;
    };
    static_assert(OperandIndex::PropDefine::object == OperandIndex::PropSet::object && OperandIndex::PropDefine::object == OperandIndex::SetLiteralPrototype::object && OperandIndex::PropDefine::object == OperandIndex::FinishObject::object);
    Vector<UseInBlock, 16> uses;
    Vector<ValueID, 2> names { allocationID };
    for (size_t name = 0; name < names.size(); ++name) {
        const Block& contents = function.at(block);
        ValueID named = names[name];
        auto addUsesOutsideValues = [&](std::span<const ValueID> used) {
            for (ValueID id : used) {
                if (id == named)
                    uses.append({ std::numeric_limits<uint32_t>::max(), { }, false });
            }
        };
        addUsesOutsideValues({ &contents.subject, 1 });
        if (contents.handler)
            addUsesOutsideValues(contents.handler->arguments.span());
        uint32_t index = 0;
        for (ValueID user : contents.values) {
            uint32_t operandIndex = 0;
            for (ValueID operand : function.at(user).operands()) {
                if (operand == named) {
                    uses.append({ index, user, operandIndex == OperandIndex::PropDefine::object });
                    if (function.at(user).opcode == Opcode::FinishObject)
                        names.append(user);
                }
                ++operandIndex;
            }
            ++index;
        }
        addUsesOutsideValues(contents.terminator.operands());
        for (const Target& target : contents.terminator.targets)
            addUsesOutsideValues(target.arguments.span());
    }
    bool hasStoreToList = uses.containsIf([&](const UseInBlock& use) {
        return use.isReceiver && storesListedAtAllocations.contains(function.at(use.user).opcode);
    });
    if (!hasStoreToList)
        return false;
    std::ranges::stable_sort(uses, { }, &UseInBlock::index);

    Vector<uint32_t, 16> fields(function.at(allocationID).fields());
    Vector<ValueID, 16> listedValues(function.at(allocationID).variadicOperands());
    Vector<PropertyKeyID, 16> writtenNames;
    BitVector isListedNow(function.values.size());
    BitVector movesBehindAllocation(function.values.size());
    Vector<ValueID, 4> waiting;
    struct Filler {
        size_t listedAt;
        Value value;
    };
    Vector<Filler, 4> fillers;
    size_t numberOfFieldsToList = fields.size();
    size_t numberOfFillersToList = 0;
    bool hasOtherPrototype = !!function.at(allocationID).operand(OperandIndex::NewObject::prototype);
    std::optional<uint32_t> lastListedStore;

    for (size_t position = 0; position < uses.size(); ++position) {
        const UseInBlock& use = uses[position];
        bool namesObjectTwice = position + 1 < uses.size() && uses[position + 1].index == use.index;
        if (!use.isReceiver || namesObjectTwice)
            break;
        const Value& user = function.at(use.user);
        if (user.opcode == Opcode::FinishObject || user.opcode == Opcode::SetLiteralPrototype) {
            hasOtherPrototype |= user.opcode == Opcode::SetLiteralPrototype;
            waiting.append(use.user);
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
        std::optional<NamedAccessSite> store = planNamedAccessSite(m_context, { block, use.index, use.user });
        bool wouldStoreToSlot = store && store->plan.form == AccessForm::Slot && store->plan.stores()
            && store->plan.receiverTest == ReceiverTest::None && store->plan.conditions.isEmpty() && (store->plan.requiredProofs | proofs) == proofs;
        if (!wouldStoreToSlot || !store->plan.storedValueFits) {
            // With something under the name the property is the object's own: no setter is looked for, and the run time looks at the value.
            if (!isListed) {
                const Field& declaration = fieldAt(m_context.program, declarationOf(m_context.program, { function.at(allocationID).layout(), *field }));
                std::optional<Value> filler;
                if (wouldStoreToSlot && Typed::canBeStoredBehindAllocation(declaration))
                    filler = Typed::tryCreateFiller(m_context.program, m_context.program.at(Typed::slotTypeOf(m_context.program, declaration)));
                if (!filler)
                    break;
                fillers.append({ fields.size(), WTF::move(*filler) });
                fields.append(*field);
                listedValues.append(ValueID());
            }
            waiting.append(use.user);
            continue;
        }
        if (isListed)
            listedValues[listedAt] = user.valueOperand();
        else {
            fields.append(*field);
            listedValues.append(user.valueOperand());
        }
        isListedNow.quickSet(use.user.index());
        for (ValueID moved : std::exchange(waiting, { }))
            movesBehindAllocation.quickSet(moved.index());
        lastListedStore = use.index;
        numberOfFieldsToList = fields.size();
        numberOfFillersToList = fillers.size();
    }
    if (!lastListedStore)
        return false;
    fields.shrink(numberOfFieldsToList);
    listedValues.shrink(numberOfFieldsToList);
    fillers.shrink(numberOfFillersToList);

    BlockValues& values = function.edit(block).values;
    Vector<ValueID, 32> reordered;
    for (uint32_t index = allocationIndex + 1; index <= *lastListedStore; ++index) {
        if (!isListedNow.quickGet(values[index].index()) && !movesBehindAllocation.quickGet(values[index].index()))
            reordered.append(values[index]);
    }
    m_builder.setOrigin(function.at(allocationID).origin());
    for (Filler& filler : fillers) {
        listedValues[filler.listedAt] = m_builder.addDetached(WTF::move(filler.value));
        reordered.append(listedValues[filler.listedAt]);
    }
    reordered.append(allocationID);
    for (uint32_t index = allocationIndex + 1; index <= *lastListedStore; ++index) {
        if (movesBehindAllocation.quickGet(values[index].index()))
            reordered.append(values[index]);
    }
    values.removeAt(allocationIndex, *lastListedStore - allocationIndex + 1);
    values.insertVector(allocationIndex, reordered);

    Value& allocation = function.edit(allocationID);
    allocation.setFields(fields.span());
    allocation.setVariadicOperands(listedValues.span());
    m_context.analyses.didChangeValues();
    return true;
}

bool AccessResolver::listFirstStoresAtAllocations()
{
    Function& function = m_context.function;
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
    for (const ValueSite& site : m_context.sitesIn(SiteList::Accesses)) {
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
                didChange |= listFirstStoresAtAllocation(block, index);
        }
    }
    return didChange;
}

PassOutcome AccessResolver::run()
{
    bool didListStores = listFirstStoresAtAllocations();
    planSites();
    if (m_namedSites.isEmpty() && m_elementSites.isEmpty() && m_readsOfFieldsThatMayBeEmpty.isEmpty())
        return didListStores ? PassOutcome::Changed : PassOutcome::Unchanged;
    Vector<ValueID> rewritten = rewriteSites();
    expandReadsOfFieldsThatMayBeEmpty();
    // Narrowing may renumber the values, and the planned sites name them.
    if (!rewritten.isEmpty())
        narrowValueTypes(TypePropagationInputs { m_context.program, m_context.id, m_context.function, m_context.analyses, m_context.types, m_context.build, &m_context.typingMemo }, rewritten.span());
    return PassOutcome::Changed;
}

// Nothing but the state of the slot is open, and either state has its own read: no lookup by name from the receiver is left.
static bool canGoOnAtPrototype(const PropertyAccessPlan& plan)
{
    return plan.emptySlotMeansNoOwnProperty && plan.receiverTest == ReceiverTest::None && plan.conditions.isEmpty() && plan.requiredProofs == ProofSet { Proof::SlotFilled };
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
    return plan.form == AccessForm::GetterInSlot && plan.receiverTest == ReceiverTest::None && plan.conditions.isEmpty() && plan.requiredProofs == ProofSet { Proof::SlotFilled };
}

static bool testsSlotOfGetterWhereItStands(FunctionAnalyses& analyses, const ValueSite& site, const PropertyAccessPlan& plan)
{
    if (!mayTestSlotOfGetterWhereItStands(plan))
        return false;
    const Function& function = analyses.function();
    BlockID block = site.block;
    if (standsBehindFailedTestOfItsSlot(analyses, site))
        return false;
    switch (function.at(block).frequency) {
    case BlockFrequency::Normal:
        return function.runsOnlyBehindFailedTest();
    case BlockFrequency::Unlikely:
        return true;
    case BlockFrequency::Rare:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool answersWithoutLookup(AccessForm form)
{
    return form == AccessForm::Constant || form == AccessForm::Builtin || form == AccessForm::Method;
}

// The plan alone says that nothing is made of the access here.
static bool isRefusedWhereverItStands(const PropertyAccessPlan& plan)
{
    return !answersWithoutLookup(plan.form) && !(plan.isResolved() && plan.needsNoTest()) && !canGoOnAtPrototype(plan) && !mayTestSlotOfGetterWhereItStands(plan);
}

void AccessResolver::planSites()
{
    PropertyAccessCache* cache = m_context.program.propertyAccessCache();
    m_context.forEachSiteIn(SiteList::Accesses, [&](const ValueSite& site) {
        uint32_t state = RefusedSites::stateOf(m_context.function, site.value);
        if (m_context.refusedSites.contains(RefusalKind::Access, site.value, state))
            return;
        auto refuse = [&] {
            m_context.refusedSites.add(RefusalKind::Access, m_context.function, site.value, state);
        };
        std::optional<SiteContent> content = cache ? contentOfNamedAccess(m_context, m_context.function.at(site.value)) : std::nullopt;
        std::optional<uint32_t> limitOfKnownRefusal = content ? cache->refusedContents.find(*content) : std::nullopt;
        if (limitOfKnownRefusal && !cache->checksAnswers) [[likely]] {
            recordLoss(site, static_cast<LimitID>(*limitOfKnownRefusal));
            refuse();
            return;
        }
        auto refuseWhereverItStands = [&](LimitID limit) {
            RELEASE_ASSERT(!limitOfKnownRefusal || *limitOfKnownRefusal == static_cast<uint32_t>(limit));
            recordLoss(site, limit);
            refuse();
            if (content && !limitOfKnownRefusal)
                cache->refusedContents.add(*content, static_cast<uint32_t>(limit));
        };
        if (std::optional<PropertyAccessQuery> query = namedAccessQueryOf(m_context, m_context.function.at(site.value))) {
            // A plan for such a receiver begins with a test of the receiver, which PlaceTypeTests places.
            if (!query->receivers.isProven()) {
                refuseWhereverItStands(limitOfPropertyAccessPlan(m_context.program, *query));
                return;
            }
            PropertyAccessContext accessContext { m_context.program, &m_context.inputs.flow };
            PropertyAccessPlan plan = planPropertyAccess(accessContext, *query);
            bool isRefused = isRefusedWhereverItStands(plan);
            if (query->planFollowsFromIsOnlyCalled && m_context.function.at(site.value).opcode == Opcode::PropGet) {
                // Who uses what is read is asked, of the lists of all uses in the function, only where the two answers lead to two ends.
                query->isOnlyCalled = true;
                PropertyAccessPlan planOfCalledRead = planPropertyAccess(accessContext, *query);
                isRefused = isRefused && isRefusedWhereverItStands(planOfCalledRead) && planOfCalledRead.limit == plan.limit;
                if (!isRefused) {
                    query->isOnlyCalled = isOnlyCalled(m_context.analyses, site.value);
                    if (query->isOnlyCalled)
                        plan = WTF::move(planOfCalledRead);
                }
            }
            if (isRefused) {
                refuseWhereverItStands(plan.limit);
                return;
            }
            RELEASE_ASSERT(!limitOfKnownRefusal);
            NamedAccessSite named { site, WTF::move(*query), WTF::move(plan) };
            if (answersWithoutLookup(named.plan.form) && isTestedByValidation(m_context.function, m_context.analyses.useLists(), site.value))
                return;
            if (named.plan.isResolved() && named.plan.needsNoTest() && named.plan.form != AccessForm::Call)
                m_namedSites.append(WTF::move(named));
            else if (canGoOnAtPrototype(named.plan) || testsSlotOfGetterWhereItStands(m_context.analyses, site, named.plan))
                m_readsOfFieldsThatMayBeEmpty.append(WTF::move(named));
            else {
                recordLoss(site, named.plan.limit);
                refuse();
            }
            return;
        }

        std::optional<ElementAccessSite> element = planElementAccessSite(m_context, site);
        if (!element) {
            refuse();
            return;
        }
        if (element->plan.isResolved() && element->plan.needsNoTest()) {
            m_elementSites.append(WTF::move(*element));
            return;
        }
        // A form that needs tests is not rewritten here: the access stays by key and learns what the type of its key proves.
        if (canMarkIntegralKey(m_context.function.at(site.value), element->plan))
            m_elementSites.append(ElementAccessSite { site, createIntegralKeyPlan() });
        else
            refuse();
    });
}

Vector<ValueID> AccessResolver::rewriteSites()
{
    Function& function = m_context.function;
    ProgramPoint current;
    // ScopedLambda passes its arguments on as lvalues.
    auto insertBeforeCurrent = [&](auto&& value) -> ValueID {
        m_builder.setOrigin(value.origin());
        return m_builder.insertBefore(current, WTF::move(value));
    };
    InsertValue insert = insertBeforeCurrent;
    AccessRewriter rewriter(m_context, insert);

    Vector<ValueID> changed;
    changed.reserveInitialCapacity(m_namedSites.size() + m_elementSites.size());
    for (const NamedAccessSite& site : m_namedSites) {
        current = site.site.point();
        rewriter.rewrite(site, operandsOfNamedAccess(function.at(site.site.value)));
        changed.append(site.site.value);
    }
    for (const ElementAccessSite& site : m_elementSites) {
        current = site.site.point();
        rewriter.rewrite(site, operandsOfElementAccess(function.at(site.site.value)));
        if (site.plan.isResolved())
            changed.append(site.site.value);
    }

    m_builder.applyInsertions();
    m_context.analyses.didChangeValues();
    return changed;
}

void AccessResolver::expandReadsOfFieldsThatMayBeEmpty()
{
    if (m_readsOfFieldsThatMayBeEmpty.isEmpty())
        return;

    // The rewrites have moved the reads, and an expansion moves what stands behind it.
    const UseLists& useLists = m_context.analyses.useLists();
    for (NamedAccessSite& read : m_readsOfFieldsThatMayBeEmpty) {
        ValueLocation location = useLists.locationOf(read.site.value);
        RELEASE_ASSERT(location.placement == ValuePlacement::InBlock);
        read.site.block = location.block;
        read.site.index = location.index;
    }
    std::ranges::sort(m_readsOfFieldsThatMayBeEmpty, std::ranges::greater { }, [](const NamedAccessSite& read) {
        return std::pair { read.site.block, read.site.index };
    });

    SiteExpansion expansion(m_context, m_builder);
    for (const NamedAccessSite& read : m_readsOfFieldsThatMayBeEmpty) {
        SlotStamp stamp = createSlotStamp(m_context, m_context.function.at(read.site.value), read.plan);
        bool testsDeclaredType = read.plan.testsDeclaredType;
        expansion.begin(read.site);
        ValueID object = expansion.original().objectOperand();
        Value test = makeObjectTest(Proof::SlotFilled, object, stamp);
        test.setHoldsDeclaredType(testsDeclaredType);
        ValueID isFilled = m_builder.append(WTF::move(test));
        BlockFrequency frequencyOfFailure = testsDeclaredType ? rarerOf(expansion.frequency(), BlockFrequency::Unlikely) : expansion.frequency();
        IRBuilder::ProvenBranch branch = m_builder.emitBranchWithProof(isFilled, OperandIndex::IsSlotFilled::object, expansion.frequency(), frequencyOfFailure);

        m_builder.moveToEndOf(branch.taken);
        if (read.plan.form == AccessForm::GetterInSlot)
            expansion.finishArm(appendCallOfGetterInSlot(read.plan, stamp, object, branch.provenOnTaken));
        else {
            Value ownRead = IRBuilder::make(Opcode::GetSlot, { object, branch.provenOnTaken });
            setSlotStamp(ownRead, stamp);
            ownRead.setHoldsDeclaredType(testsDeclaredType);
            expansion.finishArm(expansion.append(WTF::move(ownRead)));
        }

        m_builder.moveToEndOf(branch.notTaken);
        // The lookup starts over at the object. A slot that holds undefined or null fails the test for a value of the field's type as an empty slot does.
        // And of a lookup that starts at the object its type says what it may find: it runs no more code than the read in whose place it stands, which the callers of this function have relied on.
        Value lookup = IRBuilder::make(Opcode::SuperGet, { object, object });
        lookup.setKey(stamp.key);
        expansion.finishArm(expansion.append(WTF::move(lookup)));
        expansion.end();
    }
    m_context.analyses.didChangeValues();
}

ValueID AccessResolver::appendCallOfGetterInSlot(const PropertyAccessPlan& plan, const SlotStamp& stamp, ValueID object, ValueID guard)
{
    const Program& program = m_context.program;
    Value read = IRBuilder::make(Opcode::GetSlotGetter, { object, guard });
    setSlotStamp(read, { stamp.key, stamp.receivers, stamp.level, { } });
    ValueID getter = m_builder.append(WTF::move(read));

    std::optional<Vector<FunctionID, 2>> getters = gettersInSlots(program, plan.receivers.span(), stamp.key);
    if (getters && getters->size() == 1 && mayBeCalledDirectly(program, getters->first())) {
        FunctionID callee = getters->first();
        Vector<ValueID, 8> captures = appendCaptures(m_builder, callee, getter);
        return appendDirectCall(m_builder, {
            .callee = callee,
            .captures = captures.span(),
            .thisValue = object,
            .calleeValue = program.at(callee).has(FunctionFlags::TakesCallee) ? getter : ValueID(),
            .accessSite = stamp.accessSite,
        });
    }
    Value call = IRBuilder::make(Opcode::Call);
    call.setCalleeOperand(getter);
    call.setThisValueOperand(object);
    call.setAccessSite(stamp.accessSite);
    return m_builder.append(WTF::move(call));
}

void AccessResolver::recordLoss(const ValueSite& site, LimitID limit)
{
    if (limit != LimitID::None)
        m_context.gaveUp(limit, site.value);
}

PassOutcome resolveAccesses(FunctionPassContext& context)
{
    PassOutcome outcome = AccessResolver(context).run();
    // Each step tells the analyses of its edits as it makes them.
    if (outcome == PassOutcome::Changed)
        context.keptAnalyses = allAnalyses;
    return outcome;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
