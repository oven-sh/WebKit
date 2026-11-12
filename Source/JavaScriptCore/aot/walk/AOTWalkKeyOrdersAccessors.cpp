/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTBuiltinMembers.h"
#include "AOTBuiltinTable.h"
#include "AOTFamilies.h"
#include "AOTIRProgram.h"
#include "AOTLayout.h"
#include "AOTLinkTables.h"
#include "AOTNameClass.h"
#include "AOTTypeArena.h"
#include "AOTWalkKnown.h"
#include "ProxyObject.h"
#include <algorithm>
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t closedOperationsOfAccessThatCallsAccessorInKeyOrdersAccessors = 2;

static ALWAYS_INLINE LayoutID onlyLayoutOfInKeyOrdersAccessors(const Walker& walker, TypeID listed)
{
    if (!listed)
        return { };
    Type type = walker.types().at(listed);
    if (!type.isJavaScriptValue() || type.primitives || !type.hasOnlyLayouts() || type.traitsOfNamed.contains(ObjectTrait::MayHaveLayoutBelow) || !holdsOneMember(type.layouts))
        return { };
    return singleLayoutOf(walker.types(), type);
}

static std::optional<FieldReference> findAccessorSlotInKeyOrdersAccessors(const Walker& walker, LayoutID layout, PropertyKeyID key)
{
    const Program& program = walker.program();
    const Plan& plan = walker.plan();
    if (!layout || !key || liesInThread(key) || program.at(key).kind != PropertyKeyKind::Text || program.isIndexLike(key) || program.nameClassOf(key) == NameClass::Proto)
        return std::nullopt;
    if (!isFoundByShapeTest(program.at(layout)) || !plan.isInUse(layout) || !plan.slots().holdsCompiledObjects(layout))
        return std::nullopt;
    std::optional<FieldIndex> field = plan.slots().findField(program, layout, key);
    if (!field || !plan.slot(layout, *field).holdsAccessor || !plan.slot(layout, *field).hasStorage())
        return std::nullopt;
    return FieldReference { layout, *field };
}

static FunctionID findAccessorExpectedInKeyOrdersAccessors(const Walker& walker, FieldReference field, bool isStore)
{
    const LinkTables& linked = walker.facts().linked();
    const LinkedAccessorsOfField* expected = linked.findAccessorsExpectedIn(field);
    FunctionID accessor = expected ? (isStore ? expected->setter : expected->getter) : FunctionID();
    return accessor && linked.mayBeMentionedBy(walker.unit(), accessor) ? accessor : FunctionID();
}

static bool isProvenHalfInKeyOrdersAccessors(const Walker& walker, FieldReference field, bool isStore)
{
    const LinkedAccessorsOfField* laid = walker.facts().linked().findAccessorsExpectedIn(field);
    return laid && (isStore ? laid->setterIsProven : laid->getterIsProven);
}

static bool halfMayNotBeLaidYetInKeyOrdersAccessors(const Walker& walker, FieldReference field, bool isStore)
{
    const LinkedAccessorsOfField* laid = walker.facts().linked().findAccessorsExpectedIn(field);
    return laid && !isStore && laid->getterMayNotBeLaidYet;
}

static void checkProvenHalfInKeyOrdersAccessors(Walker& walker, const BodyValue& access, Reference object, FieldReference field)
{
    if (!walker.checksProofs())
        return;
    bool isStore = access.opcode() == Opcode::PropSet;
    Origin origin = walker.originOf(access);
    origin.isTailCall = false;
    Draft load(isStore ? Opcode::GetSlotSetter : Opcode::GetSlotGetter, origin);
    load.setObjectOperand(object);
    load.setKey(access.key());
    load.setReceivers(walker.types().at(walker.types().forLayout(field.layout)).layouts);
    load.setLevel(KnowledgeLevel::Proven);
    if (isStore)
        load.setIsInStrictCode(access.mode() == ECMAMode::Strict);
    walker.buildTestOfCheck(WTF::move(load));
}

bool callProvenAccessorOfItsOpcodes(Walker& walker, const BodyValue& access)
{
    bool isStore = access.opcode() == Opcode::PropSet;
    if (access.isFreeName())
        return false;
    PropertyKeyID key = access.key();
    if (!key || liesInThread(key) || !walker.facts().linked().keysOfAccessorsExpectedInFields.get(key.index()))
        return false;
    if (!walker.has(Mechanism::TypedOperations))
        return false;
    Reference object = walker.referenceOf(access.objectOperand());
    Reference stored = isStore ? walker.referenceOf(access.valueOperand()) : Reference();
    if (!object || !object.isValue() || (isStore && !stored))
        return false;
    std::optional<FieldReference> field = findAccessorSlotInKeyOrdersAccessors(walker, onlyLayoutOfInKeyOrdersAccessors(walker, walker.typeOf(object)), key);
    if (!field || !isProvenHalfInKeyOrdersAccessors(walker, *field, isStore) || halfMayNotBeLaidYetInKeyOrdersAccessors(walker, *field, isStore))
        return false;
    FunctionID half = findAccessorExpectedInKeyOrdersAccessors(walker, *field, isStore);
    if (!half)
        return false;
    checkProvenHalfInKeyOrdersAccessors(walker, access, object, *field);
    return walker.isUnreachable() || callAccessor(walker, access, half, object, stored);
}

static void fillSlotOperationInKeyOrdersAccessors(Walker& walker, Draft& draft, Reference object, FieldReference field, PropertyKeyID key)
{
    draft.setObjectOperand(object);
    draft.setKey(key);
    draft.setReceivers(walker.types().at(walker.types().forLayout(field.layout)).layouts);
    draft.setLevel(KnowledgeLevel::Proven);
}

static bool isListedUnder(const Program& program, LayoutID layout, FieldIndex field, PropertyKeyID key)
{
    const Vector<Field>& listed = program.at(layout).fields;
    return field < listed.size() && listed[field].key == key;
}

struct ProxyMadeInSight {
    ValueID made;
    ValueID target;
    ValueID handler;
};

static std::optional<ProxyMadeInSight> findProxyMadeInSight(const Walker& walker, Reference proxy)
{
    const Code& code = walker.code();
    ValueID made = proxy && proxy.isValue() ? underlyingCellOf(code, proxy.id()) : ValueID();
    if (!made || code.isConstant(made))
        return std::nullopt;
    const Operation& construction = code.at(made);
    if (construction.opcode != Opcode::CallBuiltin || construction.builtinCallForm() != BuiltinCallForm::Whole)
        return std::nullopt;
    const BuiltinTable& table = walker.program().builtins();
    const BuiltinOverload& overload = table.at(construction.overload());
    if (overload.mode != BuiltinCallMode::Construct || table.canonicalID(overload.builtin) != table.canonicalID(Builtins::Proxy))
        return std::nullopt;
    std::span<const ValueID> arguments = construction.argumentsOperands();
    if (arguments.size() != 2 || !arguments[0] || !arguments[1] || code.isConstant(arguments[0]) || code.isConstant(arguments[1]))
        return std::nullopt;
    return ProxyMadeInSight { made, arguments[0], arguments[1] };
}

static FunctionID functionWrittenUnder(const Walker& walker, PropertyKeyID key, ValueID holder, ValueID made)
{
    const Code& code = walker.code();
    ValueID cell = underlyingCellOf(code, holder);
    if (code.isConstant(cell) || code.at(cell).opcode != Opcode::NewObject)
        return { };
    FunctionID found;
    auto take = [&](ValueID value) {
        ValueID closure = value ? underlyingCellOf(code, value) : ValueID();
        found = closure && !code.isConstant(closure) && code.at(closure).opcode == Opcode::NewClosure ? code.at(closure).function() : FunctionID();
    };
    const Operation& birth = code.at(cell);
    std::span<const FieldIndex> fields = birth.fields();
    std::span<const ValueID> values = birth.variadicOperands();
    for (size_t index = 0; index < fields.size() && index < values.size(); ++index) {
        if (isListedUnder(walker.program(), birth.layout(), fields[index], key))
            take(values[index]);
    }
    for (uint32_t index = cell.index() + 1; index < made.index(); ++index) {
        const Operation& store = code.at(ValueID(index));
        if ((store.opcode == Opcode::PutSlot || store.opcode == Opcode::PutSlotOrAdd) && store.key() == key && underlyingCellOf(code, store.objectOperand()) == cell)
            take(store.valueOperand());
    }
    return found;
}

struct ProxyWrittenIntoVariable {
    LayoutID target;
    LayoutID handler;
    FunctionID trap;
    bool targetIsArray { false };
};

static std::optional<ProxyWrittenIntoVariable> findProxyWrittenIntoVariable(const Walker& walker, Reference proxy, BuiltinName trap)
{
    const Program& program = walker.program();
    const Code& code = walker.code();
    ValueID read = proxy && proxy.isValue() ? proxy.id() : ValueID();
    while (read && !code.isConstant(read) && (code.at(read).opcode == Opcode::CheckInitialized || cellPreservingOpcodes.contains(code.at(read).opcode)))
        read = code.at(read).operand(0);
    if (!read || code.isConstant(read) || code.at(read).opcode != Opcode::GetModuleVariable)
        return std::nullopt;
    VariableID variable = code.at(read).variable();
    if (variable.index() >= program.variables.size() || !program.at(variable).flags.contains(VariableFlags::IsImmutable) || !program.at(variable).module)
        return std::nullopt;
    FunctionID body = program.at(program.at(variable).module).body;
    const BodyIndex* found = body && walker.facts().linked().function(body).makesProxies ? walker.facts().bodies().find(body) : nullptr;
    if (!found)
        return std::nullopt;
    PropertyKeyID nameOfTrap = program.keyOf(trap);
    const BodyIndex& index = *found;
    auto definitionOf = [&](ValueID value, Opcode opcode) {
        ValueID behind = index.stripCasts(value);
        return !index.isParameter(behind) && index.opcodeOf(behind) == opcode ? behind : ValueID();
    };
    auto literalBehind = [&](ValueID value) {
        ValueID behind = index.stripCasts(value);
        if (ValueID finished = definitionOf(behind, Opcode::FinishObject))
            behind = index.stripCasts(index.value(finished).operand(0));
        return behind;
    };
    auto layoutOf = [&](ValueID literal) {
        if (index.isParameter(literal))
            return LayoutID();
        BodyValue made = index.value(literal);
        if (made.opcode() == Opcode::NewObject)
            return made.as<Opcode::NewObject>().layout();
        const auto* record = made.opcode() == Opcode::Data ? program.data[made.as<Opcode::Data>().data().index()].root.tryAs<DataNode::Object>() : nullptr;
        return record ? record->layout : LayoutID();
    };
    auto isArrayLiteral = [&](ValueID literal) {
        if (index.isParameter(literal))
            return false;
        BodyValue made = index.value(literal);
        return made.opcode() == Opcode::NewArray || (made.opcode() == Opcode::Data && program.data[made.as<Opcode::Data>().data().index()].root.tryAs<DataNode::Array>());
    };
    for (uint32_t number = 0; number < index.numberOfValues(); ++number) {
        ValueID id(number);
        if (index.isParameter(id) || index.opcodeOf(id) != Opcode::PutModuleVariable || index.value(id).as<Opcode::PutModuleVariable>().variable() != variable)
            continue;
        ValueID made = definitionOf(index.value(id).as<Opcode::PutModuleVariable>().valueOperand(), Opcode::Construct);
        if (!made)
            return std::nullopt;
        auto construction = index.value(made).as<Opcode::Construct>();
        const BuiltinTable& table = program.builtins();
        ValueID callee = definitionOf(construction.calleeOperand(), Opcode::Builtin);
        if (!callee || table.canonicalID(index.value(callee).as<Opcode::Builtin>().builtin()) != table.canonicalID(Builtins::Proxy) || construction.argumentsOperands().size() != 2)
            return std::nullopt;
        ValueID handler = literalBehind(construction.argumentsOperands()[1]);
        ValueID target = literalBehind(construction.argumentsOperands()[0]);
        ProxyWrittenIntoVariable written { layoutOf(target), layoutOf(handler), FunctionID(), isArrayLiteral(target) };
        if ((!written.target && !written.targetIsArray) || !written.handler || index.opcodeOf(handler) != Opcode::NewObject)
            return std::nullopt;
        auto take = [&](ValueID value) {
            ValueID closure = definitionOf(value, Opcode::NewClosure);
            written.trap = closure ? index.value(closure).as<Opcode::NewClosure>().function() : FunctionID();
        };
        BodyValue birth = index.value(handler);
        std::span<const FieldIndex> fields = birth.as<Opcode::NewObject>().fields();
        std::span<const ValueID> values = birth.variadicOperands();
        for (size_t place = 0; place < fields.size() && place < values.size(); ++place) {
            if (isListedUnder(program, written.handler, fields[place], nameOfTrap))
                take(values[place]);
        }
        for (uint32_t place = handler.index() + 1; place < made.index(); ++place) {
            ValueID define(place);
            if (index.isParameter(define) || index.opcodeOf(define) != Opcode::PropDefine)
                continue;
            auto definition = index.value(define).as<Opcode::PropDefine>();
            if (definition.key() == nameOfTrap && index.stripCasts(definition.objectOperand()) == handler)
                take(definition.valueOperand());
        }
        return written;
    }
    return std::nullopt;
}

bool isProxyMadeInSight(const Walker& walker, Reference value)
{
    return findProxyMadeInSight(walker, value) || findProxyWrittenIntoVariable(walker, value, BuiltinNames::get);
}

TypeID typeOfTargetReadThroughProxyMadeInSight(Walker& walker, Reference proxy)
{
    if (std::optional<ProxyWrittenIntoVariable> written = findProxyWrittenIntoVariable(walker, proxy, BuiltinNames::get))
        return written->trap || !written->target ? TypeID() : walker.types().forLayout(written->target);
    std::optional<ProxyMadeInSight> sight = findProxyMadeInSight(walker, proxy);
    const Code& code = walker.code();
    if (!sight || code.at(underlyingCellOf(code, sight->handler)).opcode != Opcode::NewObject || functionWrittenUnder(walker, walker.program().keyOf(BuiltinNames::get), sight->handler, sight->made))
        return { };
    return walker.typeOf(Reference::value(underlyingCellOf(code, sight->target)));
}

bool accessThroughProxyMadeInSight(Walker& walker, const BodyValue& access)
{
    static constexpr OptionSet<CellFact> factsThatBindNoAnswer { CellFact::IsExtensible, CellFact::NoExtraProperties, CellFact::NoFieldOutsideSlot };
    bool isKeyed = access.opcode() == Opcode::ElemGet;
    if (!isKeyed && access.isFreeName())
        return false;
    Reference proxy = walker.referenceOf(access.objectOperand());
    bool isStore = access.opcode() == Opcode::PropSet;
    std::optional<ProxyMadeInSight> sight = findProxyMadeInSight(walker, proxy);
    std::optional<ProxyWrittenIntoVariable> written = sight ? std::nullopt : findProxyWrittenIntoVariable(walker, proxy, isStore ? BuiltinNames::set : BuiltinNames::get);
    if (!sight && !written)
        return false;
    const Program& program = walker.program();
    PropertyKeyID nameOfTrap = program.keyOf(isStore ? BuiltinNames::set : BuiltinNames::get);
    TypeArena& types = walker.types();
    PropertyKeyID key = isKeyed ? PropertyKeyID() : access.key();
    StringID name;
    if (isKeyed) {
        Reference written = walker.referenceOf(access.propertyOperand());
        if (written && written.isConstant() && walker.code().at(written.id()).opcode == Opcode::Integer)
            name = types.internText(byteCast<uint8_t>(String::number(walker.code().at(written.id()).integer()).span8()));
    } else if (key && !liesInThread(key) && program.at(key).kind == PropertyKeyKind::Text)
        name = program.at(key).text;
    if (!name || (written && !types.overlap(walker.typeOf(proxy), types.builtinClass(BuiltinClasses::Proxy))))
        return false;
    if (!walker.has(Mechanism::Guesses) || !walker.opensGuesses() || !walker.has(Mechanism::DirectCalls) || !walker.has(Mechanism::TypedOperations))
        return false;
    const Plan& plan = walker.plan();
    Reference stored = isStore ? walker.referenceOf(access.valueOperand()) : Reference();
    if ((isStore && !stored) || walker.draftOf(access).resultIsReturnedAtOnce)
        return false;

    const Code& code = walker.code();
    Reference target = sight ? Reference::value(underlyingCellOf(code, sight->target)) : Reference();
    Reference handler = sight ? Reference::value(underlyingCellOf(code, sight->handler)) : Reference();
    Reference targetGiven = sight ? Reference::value(sight->target) : Reference();
    Reference handlerGiven = sight ? Reference::value(sight->handler) : Reference();
    auto holdsCompiledObjects = [&](LayoutID layout) {
        return layout && isFoundByShapeTest(program.at(layout)) && plan.isInUse(layout) && plan.slots().holdsCompiledObjects(layout);
    };
    TypeID arrays = types.builtinClass(BuiltinClasses::Array);
    bool targetIsArray = written ? written->targetIsArray : types.isSubset(walker.typeOf(target), arrays);
    LayoutID layoutOfTarget = targetIsArray ? LayoutID() : written ? written->target : onlyLayoutOfInKeyOrdersAccessors(walker, walker.typeOf(target));
    LayoutID layoutOfHandler = written ? written->handler : onlyLayoutOfInKeyOrdersAccessors(walker, walker.typeOf(handler));
    if ((!targetIsArray && !holdsCompiledObjects(layoutOfTarget)) || !holdsCompiledObjects(layoutOfHandler))
        return false;
    uint32_t numberOfListedFields = targetIsArray ? 0 : program.at(layoutOfTarget).fields.size();
    uint32_t numberOfFields = targetIsArray ? 0 : numberOfListedFields + walker.facts().linked().numberOfAppendedKeysOfPlan(layoutOfTarget);
    for (uint32_t number = 0; number < numberOfFields; ++number) {
        static constexpr OptionSet<FieldForm> formsThatMayBind { FieldForm::IsReadOnly, FieldForm::IsNotConfigurable };
        OptionSet<FieldForm> form = walker.facts().field(FieldReference { layoutOfTarget, FieldIndex(number) }).formOfValue;
        if (form.containsAny(formsThatMayBind) || (number < numberOfListedFields && program.at(layoutOfTarget).fields[number].form.containsAny(formsThatMayBind)))
            return false;
    }
    auto findSlotOfValue = [&](LayoutID layout, PropertyKeyID name) {
        std::optional<FieldIndex> field = plan.slots().findField(program, layout, name);
        bool holdsValue = field && !plan.slot(layout, *field).holdsAccessor && plan.slot(layout, *field).hasStorage() && plan.slot(layout, *field).representation != SlotRepresentation::DeferredClosure;
        return holdsValue ? field : std::nullopt;
    };
    FunctionID trap = written ? written->trap : functionWrittenUnder(walker, nameOfTrap, sight->handler, sight->made);
    bool hasNoTrap = !isStore && !trap && (written || code.at(handler.id()).opcode == Opcode::NewObject);
    if (hasNoTrap && (targetIsArray || isKeyed))
        return false;
    bool handlerListsName = plan.slots().findField(program, layoutOfHandler, nameOfTrap).has_value();
    std::optional<FieldIndex> fieldOfTrap = findSlotOfValue(layoutOfHandler, nameOfTrap);
    std::optional<FieldIndex> field = hasNoTrap ? findSlotOfValue(layoutOfTarget, key) : fieldOfTrap;
    if (!field || (handlerListsName && !fieldOfTrap))
        return false;
    if (hasNoTrap && (!recordFindsNothingAbove(walker, layoutOfHandler, nameOfTrap) || (!handlerListsName && std::ranges::contains(walker.facts().linked().appendedKeysOf(layoutOfHandler), nameOfTrap))))
        return false;
    if (!hasNoTrap && (!trap || !accessorAdmitsDirectCall(walker, trap) || !walker.facts().linked().mayBeMentionedBy(walker.unit(), trap)))
        return false;
    TypeID closures = hasNoTrap ? TypeID() : types.forFunction(trap);
    if (!hasNoTrap && !shouldOpenGuess(walker, proxy, closures, true, closedOperationsOfAccessThatCallsAccessorInKeyOrdersAccessors))
        return false;

    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(access.id()));
    uint32_t place = access.id().index() - block.firstValue().index();
    Origin origin = walker.originOf(access);
    origin.isTailCall = false;
    BlockID asWritten = walker.addBlock(BlockFrequency::Rare);
    BlockID meeting = walker.addBlock();
    walker.recordSpeculation();
    walker.count(StatisticID::TrapsOfProxiesCalledInSight);

    Walker::StretchInArms stretch = walker.stretchInArms(block.id(), place, place + 1);
    Walker::ArmsThatMeet arms;
    Walker::IterationPurity purity = walker.isIterationEffectFree() ? Walker::IterationPurity::EffectFree : Walker::IterationPurity::KnownEffectsOnlyWithInlinees;
    bool restarts = walker.canRestartIteration();
    bool someTestLeadsToArm = false;
    bool isAskedInFront = false;
    auto continueIfCheckPasses = [&](Draft&& test, BlockID whereTestFails, bool passesWithYes = true) {
        Reference passes = walker.isUnreachable() ? Reference() : walker.build(WTF::move(test));
        if (!passes || walker.isUnreachable())
            return Reference();
        isAskedInFront = whereTestFails == asWritten && walker.tryBranchInPreheader(passes, passesWithYes, purity);
        if (isAskedInFront)
            return passes;
        BlockID passed = walker.addBlock();
        walker.branch(passes, passesWithYes ? passed : whereTestFails, passesWithYes ? whereTestFails : passed);
        someTestLeadsToArm |= whereTestFails == asWritten;
        walker.continueIn(passed);
        return walker.isUnreachable() ? Reference() : passes;
    };
    auto walkAsWritten = [&] {
        if (!someTestLeadsToArm)
            return;
        if (!restarts)
            return walker.walkClosedArm(asWritten, stretch, arms);
        walker.continueIn(asWritten);
        if (!walker.isUnreachable())
            walker.restartIteration();
    };
    auto testThatTargetBindsNoAnswer = [&] {
        if (targetIsArray) {
            Draft test(Opcode::BindsNoTrapAnswer, origin);
            test.setTargetOperand(target);
            return test;
        }
        Draft test(Opcode::IsNotFrozen, origin);
        test.setObjectOperand(target);
        test.setIsExtensible(true);
        test.setHasNoFieldOutsideSlot(true);
        test.setHasNoExtraProperties(true);
        return test;
    };
    auto targetBindsNoAnswer = [&] {
        return !targetIsArray && walker.state().cellFacts(target).containsAll(factsThatBindNoAnswer);
    };
    Draft nameAsString(Opcode::String, origin);
    nameAsString.setString(name);
    Reference nameGiven = walker.constant(WTF::move(nameAsString));
    auto finish = [&](Reference answer) {
        Draft draft(Opcode::FinishAccessThroughProxy, origin);
        draft.setTargetOperand(targetGiven);
        draft.setAnswerOperand(answer);
        draft.setStoredOperand(stored);
        draft.setNameOperand(nameGiven);
        draft.setMode(isStore ? access.mode() : ECMAMode::Strict);
        walker.build(WTF::move(draft));
    };

    BlockID inFrontOfTests = walker.currentBlock();
    walker.letGuessFailUnasked(asWritten, origin);
    someTestLeadsToArm = walker.currentBlock() != inFrontOfTests;
    if (written) {
        auto buildProof = [&](Reference value, Reference test, uint32_t operandIndex) {
            Walker::SpecializedPreheaderScope preheaderScope(walker, isAskedInFront, purity);
            Draft proof(Opcode::Narrowed, origin);
            proof.setValueOperand(value);
            proof.setTestOperand(test);
            proof.setOperandIndex(operandIndex);
            proof.setIsOnTakenEdge(true);
            return walker.build(WTF::move(proof));
        };
        Reference cell = proxy;
        if (TypeID proxies = types.builtinClass(BuiltinClasses::Proxy); !types.isSubset(walker.typeOf(proxy), proxies)) {
            Draft test(Opcode::TypeTest, origin);
            test.setValueOperand(proxy);
            test.setTestedType(proxies);
            Reference isProxy = continueIfCheckPasses(WTF::move(test), asWritten);
            cell = isProxy ? buildProof(proxy, isProxy, OperandIndex::TypeTest::value) : Reference();
        }
        auto readField = [&](unsigned number, LayoutID layout, TypeID builtin) {
            if (!cell || walker.isUnreachable() || walker.hasFailed())
                return Reference();
            Draft read(Opcode::GetInternalField, origin);
            walker.chooseConservativeFields(read);
            read.setObjectOperand(cell);
            read.setIndex(number);
            Reference held = walker.build(WTF::move(read));
            if (!held)
                return Reference();
            if (!layout) {
                Draft test(Opcode::TypeTest, origin);
                test.setValueOperand(held);
                test.setTestedType(builtin);
                Reference hasType = continueIfCheckPasses(WTF::move(test), asWritten);
                return hasType ? buildProof(held, hasType, OperandIndex::TypeTest::value) : Reference();
            }
            Draft test(Opcode::ShapeTest, origin);
            test.setObjectOperand(held);
            test.setLayouts(types.at(types.forLayout(layout)).layouts);
            Reference hasShape = continueIfCheckPasses(WTF::move(test), asWritten);
            return hasShape ? buildProof(held, hasShape, OperandIndex::ShapeTest::object) : Reference();
        };
        target = targetGiven = readField(static_cast<unsigned>(ProxyObject::Field::Target), layoutOfTarget, arrays);
        handler = handlerGiven = target ? readField(static_cast<unsigned>(ProxyObject::Field::Handler), layoutOfHandler, TypeID()) : Reference();
        if (!handler) {
            if (!walker.hasFailed())
                walkAsWritten();
            if (!walker.hasFailed())
                walker.joinArms(stretch, arms, meeting);
            return true;
        }
    }
    if (hasNoTrap) {
        OptionSet<CellFact> known = walker.state().cellFacts(handler);
        bool goesOn = known.containsAll({ CellFact::NoExtraProperties, CellFact::NoFieldOutsideSlot });
        if (!goesOn) {
            Draft test(Opcode::HasNoExtraProperties, origin);
            test.setObjectOperand(handler);
            test.setHasNoFieldOutsideSlot(true);
            goesOn = !!continueIfCheckPasses(WTF::move(test), asWritten);
            if (goesOn)
                walker.state().recordCellFacts(handler, { CellFact::NoExtraProperties, CellFact::NoFieldOutsideSlot });
        }
        if (goesOn && handlerListsName) {
            Draft look(Opcode::IsSlotFilled, origin);
            fillSlotOperationInKeyOrdersAccessors(walker, look, handler, FieldReference { layoutOfHandler, *fieldOfTrap }, nameOfTrap);
            goesOn = !!continueIfCheckPasses(WTF::move(look), asWritten, false);
        }
        if (goesOn && !known.contains(CellFact::HasLayoutPrototype)) {
            Draft test(Opcode::HasLayoutPrototype, origin);
            test.setObjectOperand(handler);
            goesOn = !!continueIfCheckPasses(WTF::move(test), asWritten);
            if (goesOn)
                walker.state().recordCellFacts(handler, CellFact::HasLayoutPrototype);
        }
        if (goesOn) {
            Draft look(Opcode::IsSlotFilled, origin);
            fillSlotOperationInKeyOrdersAccessors(walker, look, target, FieldReference { layoutOfTarget, *field }, key);
            goesOn = !!continueIfCheckPasses(WTF::move(look), asWritten);
        }
        if (goesOn) {
            Draft load(Opcode::GetSlot, origin);
            fillSlotOperationInKeyOrdersAccessors(walker, load, target, FieldReference { layoutOfTarget, *field }, key);
            load.inputValueGiven = access.id();
            if (Reference held = walker.build(WTF::move(load))) {
                walker.bind(access.id(), held);
                walker.listArm(stretch, arms);
            }
        }
        if (walker.hasFailed())
            return true;
        walkAsWritten();
        if (!walker.hasFailed())
            walker.joinArms(stretch, arms, meeting);
        return true;
    }
    bool goesOn = targetBindsNoAnswer() || !!continueIfCheckPasses(testThatTargetBindsNoAnswer(), asWritten);
    if (goesOn) {
        if (!targetIsArray)
            walker.state().recordCellFacts(target, factsThatBindNoAnswer | OptionSet<CellFact> { CellFact::NotFrozen });
        Draft look(Opcode::IsSlotFilled, origin);
        fillSlotOperationInKeyOrdersAccessors(walker, look, handler, FieldReference { layoutOfHandler, *field }, nameOfTrap);
        goesOn = !!continueIfCheckPasses(WTF::move(look), asWritten);
    }
    Reference closure;
    if (goesOn) {
        Draft load(Opcode::GetSlot, origin);
        fillSlotOperationInKeyOrdersAccessors(walker, load, handler, FieldReference { layoutOfHandler, *field }, nameOfTrap);
        Reference held = walker.build(WTF::move(load));
        Draft test(Opcode::TypeTest, origin);
        test.setValueOperand(held);
        test.setTestedType(closures);
        Reference isTrap = held ? continueIfCheckPasses(WTF::move(test), asWritten) : Reference();
        if (isTrap) {
            Walker::SpecializedPreheaderScope preheaderScope(walker, isAskedInFront, purity);
            Draft proof(Opcode::Narrowed, origin);
            proof.setValueOperand(held);
            proof.setTestOperand(isTrap);
            proof.setOperandIndex(OperandIndex::TypeTest::value);
            proof.setIsOnTakenEdge(true);
            closure = walker.build(WTF::move(proof));
        }
    }
    if (walker.hasFailed())
        return true;

    if (closure) {
        Vector<Reference, 4> arguments { targetGiven, nameGiven };
        if (isStore)
            arguments.append(stored);
        arguments.append(proxy);
        Reference answer;
        {
            Walker::StandIn proxyStandsForItself(walker, access.objectOperand(), proxy);
            if (callTrapOfProxy(walker, access, trap, handlerGiven, arguments.span(), closure))
                answer = walker.isUnreachable() || walker.hasFailed() ? Reference() : walker.referenceBoundTo(access.id());
            else {
                Draft call(Opcode::Call, origin);
                call.setCalleeOperand(closure);
                call.setThisValueOperand(handlerGiven);
                call.setArgumentsOperands(arguments.span());
                answer = walker.build(WTF::move(call));
            }
        }
        if (walker.hasFailed())
            return true;
        if (isStore && answer && !walker.isUnreachable() && !types.isSubset(walker.typeOf(answer), TypeArena::fromPrimitives(Primitive::True))) {
            Draft truth(Opcode::ToBoolean, origin);
            walker.chooseConservativeFields(truth);
            truth.setValueOperand(answer);
            BlockID saysNo = walker.addBlock(BlockFrequency::Rare);
            BlockID hasAnswered = walker.addBlock();
            Reference isTrue = walker.build(WTF::move(truth));
            if (isTrue && !walker.isUnreachable()) {
                walker.branch(isTrue, hasAnswered, saysNo);
                walker.continueIn(saysNo);
                if (!walker.isUnreachable() && walker.canRestartIteration())
                    walker.restartIteration();
                else if (!walker.isUnreachable()) {
                    finish(answer);
                    walker.listArm(stretch, arms);
                }
                if (walker.hasFailed())
                    return true;
                walker.continueIn(hasAnswered);
            }
        }
        if (answer && !walker.isUnreachable() && !targetBindsNoAnswer()) {
            BlockID binds = walker.addBlock(BlockFrequency::Rare);
            if (!!continueIfCheckPasses(testThatTargetBindsNoAnswer(), binds)) {
                if (!isStore)
                    walker.bind(access.id(), answer);
                walker.listArm(stretch, arms);
                walker.continueIn(binds);
                if (!walker.isUnreachable() && walker.canRestartIteration())
                    walker.restartIteration();
                else if (!walker.isUnreachable()) {
                    finish(answer);
                    if (!isStore && !walker.isUnreachable())
                        walker.bind(access.id(), answer);
                    walker.listArm(stretch, arms);
                }
                answer = { };
            }
        }
        if (answer && !walker.isUnreachable()) {
            if (!isStore)
                walker.bind(access.id(), answer);
            walker.listArm(stretch, arms);
        }
        if (walker.hasFailed())
            return true;
    }
    walkAsWritten();
    if (!walker.hasFailed())
        walker.joinArms(stretch, arms, meeting);
    return true;
}

bool callAccessorInSlot(Walker& walker, const BodyValue& access)
{
    Opcode opcode = access.opcode();
    bool isStore = opcode == Opcode::PropSet;
    if ((opcode != Opcode::PropGet && !isStore) || access.isFreeName())
        return false;
    if (PropertyKeyID name = access.key(); !name || liesInThread(name) || !walker.facts().linked().keysOfAccessorsExpectedInFields.get(name.index()))
        return false;
    if (!walker.has(Mechanism::Guesses) || !walker.opensGuesses() || !walker.has(Mechanism::DirectCalls) || !walker.has(Mechanism::TypedOperations))
        return false;
    Reference object = walker.referenceOf(access.objectOperand());
    Reference stored = isStore ? walker.referenceOf(access.valueOperand()) : Reference();
    if (!object || !object.isValue() || (isStore && !stored))
        return false;

    TypeArena& types = walker.types();
    TypeID proven = walker.typeOf(object);
    LayoutID layout = onlyLayoutOfInKeyOrdersAccessors(walker, proven);
    bool layoutIsProven = !!layout;
    if (!layoutIsProven) {
        ExpectationOfValue expectation = findExpectation(walker, access.objectOperand());
        TypeID expected = expectation.type && expectation.isDeclared ? types.withoutNullish(expectation.type) : TypeID();
        if (!expected || !proven || !types.overlap(proven, expected))
            return false;
        layout = onlyLayoutOfInKeyOrdersAccessors(walker, expected);
    }
    PropertyKeyID key = access.key();
    std::optional<FieldReference> field = findAccessorSlotInKeyOrdersAccessors(walker, layout, key);
    bool halfIsProven = field && isProvenHalfInKeyOrdersAccessors(walker, *field, isStore);
    FunctionID provenHalf = halfIsProven ? findAccessorExpectedInKeyOrdersAccessors(walker, *field, isStore) : FunctionID();
    bool slotIsFilled = provenHalf && !halfMayNotBeLaidYetInKeyOrdersAccessors(walker, *field, isStore);
    if (!field || !shouldOpenGuess(walker, object, types.forLayout(layout), true, closedOperationsOfAccessThatCallsAccessorInKeyOrdersAccessors))
        return false;

    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(access.id()));
    uint32_t place = access.id().index() - block.firstValue().index();
    Origin origin = walker.originOf(access);
    origin.isTailCall = false;

    BlockID asWritten = walker.addBlock(BlockFrequency::Rare);
    BlockID meeting = walker.addBlock();
    walker.recordSpeculation();

    Walker::StretchInArms stretch = walker.stretchInArms(block.id(), place, place + 1);
    Walker::ArmsThatMeet arms;
    auto continueIfCheckPasses = [&](Reference test, BlockID whereTestFails) {
        if (!test)
            return false;
        BlockID passed = walker.addBlock();
        walker.branch(test, passed, whereTestFails);
        walker.continueIn(passed);
        return !walker.isUnreachable();
    };
    auto buildProof = [&](Reference value, Reference test, uint32_t operandIndex) {
        Draft proof(Opcode::Narrowed, origin);
        proof.setValueOperand(value);
        proof.setTestOperand(test);
        proof.setOperandIndex(operandIndex);
        proof.setIsOnTakenEdge(true);
        return walker.build(WTF::move(proof));
    };
    auto callByValue = [&](Reference half) {
        Draft call(Opcode::Call, origin);
        if (!isStore)
            call.inputValueGiven = access.id();
        call.setCalleeOperand(half);
        call.setThisValueOperand(object);
        if (isStore)
            call.setArgumentsOperands(singleElementSpan(stored));
        Reference called = walker.build(WTF::move(call));
        if (!isStore)
            walker.bind(access.id(), called);
        walker.listArm(stretch, arms);
    };

    walker.letGuessFailUnasked(asWritten, origin);
    bool goesOn = true;
    if (!layoutIsProven) {
        Draft test(Opcode::ShapeTest, origin);
        test.setObjectOperand(object);
        test.setLayouts(types.at(types.forLayout(layout)).layouts);
        Reference hasShape = walker.build(WTF::move(test));
        goesOn = continueIfCheckPasses(hasShape, asWritten);
        if (goesOn) {
            object = buildProof(object, hasShape, OperandIndex::ShapeTest::object);
            goesOn = !!object;
            if (goesOn && slotIsFilled)
                checkProvenHalfInKeyOrdersAccessors(walker, access, object, *field);
            if (goesOn && slotIsFilled && callAccessor(walker, access, provenHalf, object, stored)) {
                walker.listArm(stretch, arms);
                goesOn = false;
            }
        }
    }
    if (goesOn) {
        Draft look(Opcode::IsSlotFilled, origin);
        fillSlotOperationInKeyOrdersAccessors(walker, look, object, *field, key);
        goesOn = continueIfCheckPasses(walker.build(WTF::move(look)), asWritten);
    }
    Reference half;
    if (goesOn) {
        Draft load(isStore ? Opcode::GetSlotSetter : Opcode::GetSlotGetter, origin);
        fillSlotOperationInKeyOrdersAccessors(walker, load, object, *field, key);
        if (isStore)
            load.setIsInStrictCode(access.mode() == ECMAMode::Strict);
        else {
            if (!halfIsProven)
                load.representation = Representation::JSValue;
        }
        half = walker.build(WTF::move(load));
    }
    if (walker.hasFailed())
        return true;

    if (half && provenHalf) {
        if (callAccessor(walker, access, provenHalf, object, stored, half))
            walker.listArm(stretch, arms);
        else
            callByValue(half);
        if (walker.hasFailed())
            return true;
        half = { };
    }

    if (half) {
        FunctionID expected = findAccessorExpectedInKeyOrdersAccessors(walker, *field, isStore);
        BlockID otherFunction = expected ? walker.addBlock(BlockFrequency::Unlikely) : BlockID();
        Reference isExpected;
        if (expected) {
            Draft test(Opcode::TypeTest, origin);
            test.setValueOperand(half);
            test.setTestedType(types.forFunction(expected));
            isExpected = walker.build(WTF::move(test));
        }
        if (isExpected) {
            Reference closure = continueIfCheckPasses(isExpected, otherFunction) ? buildProof(half, isExpected, OperandIndex::TypeTest::value) : Reference();
            if (closure && callAccessor(walker, access, expected, object, stored, closure))
                walker.listArm(stretch, arms);
            else if (closure)
                callByValue(closure);
            if (walker.hasFailed())
                return true;
            walker.continueIn(otherFunction);
        }
        if (!walker.isUnreachable())
            callByValue(half);
        if (walker.hasFailed())
            return true;
    }

    walker.walkClosedArm(asWritten, stretch, arms);
    if (!walker.hasFailed())
        walker.joinArms(stretch, arms, meeting);
    return true;
}

static LayoutID findClassOfReceiverInKeyOrdersAccessors(const Walker& walker, TypeID receiver, PropertyKeyID key)
{
    const Program& program = walker.program();
    if (!receiver || !key || liesInThread(key) || program.at(key).kind != PropertyKeyKind::Text)
        return { };
    TypeArena& types = walker.types();
    Type type = types.at(receiver);
    if (!type.isObject() || type.primitives || !type.hasOnlyClosures())
        return { };
    FunctionID constructor = singleFunctionOf(types, type);
    return constructor ? classOfClassObject(program, constructor) : LayoutID();
}

static VariableID findVariableOfStaticFieldInKeyOrdersAccessors(Walker& walker, LayoutID classLayout, PropertyKeyID key)
{
    const Program& program = walker.program();
    if (!classLayout || !walker.facts().isEvaluatedAtMostOnce(classLayout) || !walker.plan().slots().classCreation(classLayout).createsClassObjectOnMutation)
        return { };
    const Layout& record = program.at(classLayout);
    std::optional<uint32_t> field = record.findStaticField(key);
    if (!field)
        return { };
    if (classHolderIsMadeWithName(program, ClassSide::Static, key))
        return { };
    VariableID variable = record.staticFields[*field].variable;
    PlannedVariable planned = walker.plan().variable(variable);
    if (!planned.hasWordInStatics() || planned.encoding != VariableEncoding::Plain || planned.representation != SlotRepresentation::JSValue)
        return { };
    return variable;
}

static VariableID findVariableOfStaticFieldInKeyOrdersAccessors(Walker& walker, TypeID receiver, PropertyKeyID key)
{
    return findVariableOfStaticFieldInKeyOrdersAccessors(walker, findClassOfReceiverInKeyOrdersAccessors(walker, receiver, key), key);
}

static VariableID findVariableOfInheritedStaticField(Walker& walker, LayoutID heir, PropertyKeyID key)
{
    const Program& program = walker.program();
    size_t numberOfClassesLeft = program.layouts.size();
    for (LayoutID current = heir; current && numberOfClassesLeft--;) {
        if (program.at(current).findStaticField(key))
            return current == heir ? VariableID() : findVariableOfStaticFieldInKeyOrdersAccessors(walker, current, key);
        if (program.at(current).has(LayoutFlags::IsConstructorFunction) || !walker.plan().hasClassRecord(current) || walker.facts().linked().layout(current).namesAreDefinedPastLists[static_cast<size_t>(ClassSide::Static)] || walker.facts().hasMemberUnder(current, ClassSide::Static, key) || walker.facts().hasMemberUnder(current, ClassSide::Static, { }))
            return { };
        LayoutID parent;
        if (!findParentNamedByClassRecordForTypedAccess(walker, current, parent))
            return { };
        current = parent;
    }
    return { };
}

static void nameStaticFieldInEffects(const Program& program, Opcode opcode, PropertyKeyID key, Effects& effects)
{
    static_assert(declaredBoundOf(Opcode::PutStaticField).writes.containsOnly(AbstractHeapSet { AbstractHeap::NamedProperties }) && !declaredBoundOf(Opcode::PutStaticField).mayRunOtherCode());
    MemoryLocation property = MemoryLocation::objectField(key);
    if (opcode == Opcode::GetStaticFieldOrAbsent) {
        effects.reads.add(program, property);
        return;
    }
    effects.writes.remove(AbstractHeapSet { AbstractHeap::NamedProperties });
    effects.writes.add(program, property);
}

bool readsStaticMethod(const Walker& walker, const Draft& read)
{
    PropertyKeyID key = read.key();
    LayoutID classLayout = findClassOfReceiverInKeyOrdersAccessors(walker, walker.typeOf(read.objectOperand()), key);
    const LinkTables& linked = walker.facts().linked();
    if (!classLayout || !walker.plan().hasClassRecord(classLayout) || !walker.facts().isEvaluatedAtMostOnce(classLayout) || linked.keysOfStaticFields.get(key.index()))
        return false;
    return linked.findMember(walker.program(), classLayout, ClassSide::Static, key).kind == MemberFound::Kind::Method;
}

void narrowStaticFieldEffects(Walker& walker, const Draft& draft, Effects& effects)
{
    if (draft.opcode == Opcode::GetStaticFieldOrAbsent && draft.findsProperty()) {
        effects.reads.remove(AbstractHeapSet { AbstractHeap::NamedProperties, AbstractHeap::ClassObjects });
        effects.reads.add(walker.program(), MemoryLocation::objectField(draft.key()));
        effects.reads.add(walker.program(), MemoryLocation::at(AbstractHeap::NamedProperties, draft.key().index()));
        return;
    }
    nameStaticFieldInEffects(walker.program(), draft.opcode, draft.key(), effects);
}

std::optional<Effects> effectsOfStaticFieldAccess(Walker& walker, const BodyValue& access, TypeID holder)
{
    if (access.isFreeName() || !walker.has(Mechanism::Guesses) || !walker.opensGuesses())
        return std::nullopt;
    PropertyKeyID key = access.key();
    if (!key || liesInThread(key) || !walker.facts().linked().keysOfStaticFields.get(key.index()) || !findVariableOfStaticFieldInKeyOrdersAccessors(walker, holder, key))
        return std::nullopt;
    Effects effects = declaredBoundOf(Opcode::GetStaticFieldOrAbsent);
    nameStaticFieldInEffects(walker.program(), Opcode::GetStaticFieldOrAbsent, key, effects);
    if (access.opcode() == Opcode::PropSet) {
        Effects ofStore = declaredBoundOf(Opcode::PutStaticField);
        nameStaticFieldInEffects(walker.program(), Opcode::PutStaticField, key, ofStore);
        effects.merge(ofStore);
    }
    return effects;
}

static bool fieldStillLiesWhereArmHasSeenIt(Walker& walker, Walker::StaticFieldSeenInVariable& seen)
{
    const Code& code = walker.code();
    uint32_t oldest = seen.blockWhereSeen.index();
    BitVector isOnWays;
    Vector<BlockID, 8> blocksOnWays;
    auto takeOntoWays = [&](BlockID block) {
        if (block.index() < oldest)
            return false;
        if (!isOnWays.set(block.index() - oldest))
            blocksOnWays.append(block);
        return true;
    };
    auto everyWayComesFromWhereItWasSeen = [&] {
        if (!takeOntoWays(walker.currentBlock()))
            return false;
        for (size_t next = 0; next < blocksOnWays.size(); ++next) {
            BlockID block = blocksOnWays[next];
            if (block == seen.blockWhereSeen)
                continue;
            if (!code.at(block).firstEdgeIn() || code.at(block).isLoopHead || code.at(block).isHandlerBlock() || code.at(block).beliefAheadOfEdges != BeliefAheadOfEdges::NoEdgeIsAwaited)
                return false;
            for (EdgeID edge = code.at(block).firstEdgeIn(); edge; edge = code.at(edge).nextInto()) {
                BlockID from = code.at(edge).from;
                if (code.at(from).handler == edge || !takeOntoWays(from))
                    return false;
            }
        }
        return isOnWays.get(0);
    };
    auto nothingBuiltOnWaysReachesField = [&] {
        for (uint32_t built = seen.firstOperationBehind; built < code.numberOfValues(); ++built) {
            const Operation& operation = code.at(ValueID(built));
            if (operation.opcode == Opcode::GetModuleVariable || !operation.block() || operation.block().index() < oldest || !isOnWays.get(operation.block().index() - oldest))
                continue;
            if (operation.opcode == Opcode::PutStaticField) {
                if (operation.variable() == seen.variable)
                    return false;
                continue;
            }
            EffectTraits traits = operation.traits();
            if (traits.mayRunOtherCode() || traits.mayCollect() || traits.mayBeTerminated() || traits.mayWriteOutsideObjects())
                return false;
            if (traits.mayTouchMemory() && declaredEffectsOfOperation(walker.program(), operation.opcode, operation).writes.containsAny(bookkeepingHeaps))
                return false;
        }
        return true;
    };
    if (everyWayComesFromWhereItWasSeen() && nothingBuiltOnWaysReachesField())
        return true;
    seen.variable = { };
    return false;
}

static bool namesSameHolderInKeyOrdersAccessors(const BodyIndex& index, ValueID one, ValueID other)
{
    auto holder = [&](ValueID named) {
        named = index.stripCasts(named);
        return !index.isParameter(named) && index.opcodeOf(named) == Opcode::CheckInitialized ? index.stripCasts(index.value(named).operand(0)) : named;
    };
    one = holder(one);
    other = holder(other);
    if (one == other)
        return true;
    if (index.isParameter(one) || index.isParameter(other) || index.opcodeOf(one) != Opcode::GetModuleVariable || index.opcodeOf(other) != Opcode::GetModuleVariable)
        return false;
    return index.value(one).variable() == index.value(other).variable();
}

static void noteStaticField(Walker& walker, KnownNote note, VariableID variable, PropertyKeyID key, Reference held)
{
    if (!held || walker.isUnreachable() || walker.hasFailed())
        return;
    LocationMasks restsOn(AbstractHeapSet { AbstractHeap::NamedProperties, AbstractHeap::ClassObjects });
    restsOn.add(walker.program(), MemoryLocation::objectField(key));
    noteKnown(walker, note, { }, { }, variable.index(), restsOn, held.bits());
}

static void noteStaticFieldInVariable(Walker& walker, VariableID variable, PropertyKeyID key, Reference held)
{
    noteStaticField(walker, KnownNote::StaticFieldInVariable, variable, key, held);
}

static std::optional<uint64_t> findStaticFieldValue(Walker& walker, VariableID variable)
{
    if (std::optional<uint64_t> held = findKnown(walker, KnownNote::StaticFieldInVariable, { }, { }, variable.index()))
        return held;
    return findKnown(walker, KnownNote::StaticFieldValue, { }, { }, variable.index());
}

static void storeStaticFieldInVariable(Walker& walker, Reference object, VariableID variable, PropertyKeyID key, Reference stored, Origin origin)
{
    if (std::optional<uint64_t> known = walker.isInTrialVisit() ? findKnown(walker, KnownNote::StaticFieldInVariable, { }, { }, variable.index()) : std::nullopt)
        walker.requestCarriedField(object, key, Reference::fromBits(static_cast<uint32_t>(*known)), variable);
    if (walker.canRecordUndoableStore()) {
        std::optional<uint64_t> known = findKnown(walker, KnownNote::StaticFieldInVariable, { }, { }, variable.index());
        Reference before = known ? Reference::fromBits(static_cast<uint32_t>(*known)) : walker.readInitializedStaticField(object, variable, key, origin);
        if (!before || walker.hasFailed())
            return;
        walker.recordUndoableStaticStore(variable, key, object, before);
    }
    Draft store(Opcode::PutStaticField, origin);
    store.setVariable(variable);
    store.setKey(key);
    store.setClassObjectOperand(object);
    store.setValueOperand(stored);
    walker.build(WTF::move(store));
    walker.takePendingUndoableStore();
    noteStaticFieldInVariable(walker, variable, key, stored);
}

static void storeWhereArmHasSeenStaticField(Walker& walker, Reference object, VariableID variable, PropertyKeyID key, Reference before, Reference stored, Origin origin)
{
    if (before && walker.canRecordUndoableStore())
        walker.recordUndoableStaticStore(variable, key, object, before);
    Draft store(Opcode::PutStaticField, origin);
    store.setVariable(variable);
    store.setKey(key);
    store.setClassObjectOperand(object);
    store.setValueOperand(stored);
    walker.build(WTF::move(store));
    walker.takePendingUndoableStore();
}

static bool isReadOffFilledClass(Walker& walker, const BodyValue& access)
{
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID holder = index.stripCasts(access.objectOperand());
    if (holder && !index.isParameter(holder) && index.opcodeOf(holder) == Opcode::CheckInitialized)
        holder = index.stripCasts(index.value(holder).operand(0));
    if (!holder || index.isParameter(holder))
        return false;
    if (index.opcodeOf(holder) == Opcode::GetModuleVariable)
        return walker.facts().linked().variablesOfFilledClasses.get(index.value(holder).variable().index());
    if (index.opcodeOf(holder) != Opcode::FinishClass)
        return false;
    BodyBlock made = index.block(index.blockOf(holder));
    if (made.id() != index.blockOf(access.id()) && !index.dominates(made.id(), index.blockOf(access.id())))
        return false;
    for (uint32_t place = holder.index() - made.firstValue().index() + 1; place < made.numberOfValues(); ++place) {
        BodyValue operation = index.value(ValueID(made.firstValue().index() + place));
        if (operation.id() == access.id())
            return false;
        if (operation.opcode() == Opcode::CallDirect && operation.numberOfVariadicOperands() && index.stripCasts(operation.operand(0)) == holder)
            return walker.program().at(operation.function()).kind == FunctionKind::Initializer;
    }
    return false;
}

bool readStaticFieldThatIsThere(Walker& walker, const BodyValue& access)
{
    const LinkTables& linked = walker.facts().linked();
    PropertyKeyID key = access.key();
    if (access.isFreeName())
        return false;
    Reference object = walker.referenceOf(access.objectOperand());
    VariableID variable = object && object.isValue() ? findVariableOfStaticFieldInKeyOrdersAccessors(walker, walker.typeOf(object), key) : VariableID();
    if (!variable)
        return false;
    if (!walker.checksProofs() && walker.has(Mechanism::Folding)) {
        Reference only = walker.onlyValueOfType(linked.unitOfStaticField(variable));
        if (std::optional<double> number = linked.numberOfStaticField(variable))
            only = walker.constant(PrimitiveKnownToFolding::number(*number));
        if (only) {
            walker.bind(access.id(), only);
            return true;
        }
    }
    const LinkedStaticField* field = linked.findStaticField(variable);
    if (!field || (!field->isFilledAtEveryRead && !isReadOffFilledClass(walker, access)))
        return false;
    if (std::optional<uint64_t> known = findStaticFieldValue(walker, variable)) {
        walker.bind(access.id(), Reference::fromBits(static_cast<uint32_t>(*known)));
        return true;
    }
    Origin origin = walker.originOf(access);
    origin.isTailCall = false;
    constexpr Walker::IterationPurity purity = Walker::IterationPurity::KnownEffectsOnly;
    bool isReadBeforeLoop = walker.iterationsKeepStaticFieldsInVariables() && !walker.iterationsStoreUnderKey(key) && walker.isInvariantInSpecializedVersion(object, purity);
    Reference held;
    {
        Walker::SpecializedPreheaderScope preheaderScope(walker, isReadBeforeLoop, purity);
        Draft read(Opcode::GetStaticFieldOrAbsent, origin);
        read.setVariable(variable);
        read.setKey(key);
        read.setValueType(field->type);
        read.setClassObjectOperand(object);
        read.setFindsProperty(true);
        held = walker.build(WTF::move(read));
        if (!held || walker.hasFailed())
            return true;
        walker.checkHoistedRead(held, field->type, ProofSource::Variable);
    }
    if (walker.hasFailed())
        return true;
    noteStaticField(walker, KnownNote::StaticFieldValue, variable, key, held);
    walker.bind(access.id(), held);
    return true;
}

static bool isCounted(const BodyIndex& index, const BodyBlock& block, const BodyValue& read, uint32_t end)
{
    static constexpr OpcodeSet steps { Opcode::Increment, Opcode::Decrement };
    static constexpr OpcodeSet sums { Opcode::Add, Opcode::Sub };
    ValueID counted;
    auto isRead = [&](ValueID value) {
        value = index.stripCasts(value);
        return value == read.id() || (value && !index.isParameter(value) && index.opcodeOf(value) == Opcode::ToNumeric && index.stripCasts(index.value(value).operand(0)) == read.id());
    };
    for (uint32_t place = read.id().index() - block.firstValue().index() + 1; place < end; ++place) {
        BodyValue operation = index.value(ValueID(block.firstValue().index() + place));
        if (steps.contains(operation.opcode()) && isRead(operation.operand(0)))
            counted = operation.id();
        else if (sums.contains(operation.opcode()) && index.stripCasts(operation.operand(0)) == read.id()) {
            ValueID step = index.stripCasts(operation.operand(1));
            if (!index.isParameter(step) && index.opcodeOf(step) == Opcode::Integer)
                counted = operation.id();
        } else if (operation.opcode() == Opcode::PropSet && operation.key() == read.key() && counted && index.stripCasts(operation.valueOperand()) == counted)
            return true;
    }
    return false;
}

static bool isStoredOffSameHolder(const BodyIndex& index, const BodyValue& read)
{
    if (!index.holdsAnyOf(OpcodeSet { Opcode::PropSet }))
        return false;
    for (uint32_t number = 0; number < index.numberOfBlocks(); ++number) {
        for (BodyValue operation : index.block(BlockID(number)).values()) {
            if (operation.opcode() == Opcode::PropSet && !operation.isFreeName() && operation.key() == read.key() && namesSameHolderInKeyOrdersAccessors(index, operation.objectOperand(), read.objectOperand()))
                return true;
        }
    }
    return false;
}

bool accessStaticFieldInVariable(Walker& walker, const BodyValue& access)
{
    Opcode opcode = access.opcode();
    bool isStore = opcode == Opcode::PropSet;
    if ((opcode != Opcode::PropGet && !isStore) || access.isFreeName())
        return false;
    if (PropertyKeyID name = access.key(); !name || liesInThread(name) || !walker.facts().linked().keysOfStaticFields.get(name.index()))
        return false;
    if (!walker.has(Mechanism::Guesses) || !walker.opensGuesses() || !walker.has(Mechanism::TypedOperations))
        return false;
    Reference object = walker.referenceOf(access.objectOperand());
    Reference stored = isStore ? walker.referenceOf(access.valueOperand()) : Reference();
    if (!object || !object.isValue() || (isStore && !stored))
        return false;
    TypeArena& types = walker.types();
    VariableID variable = findVariableOfStaticFieldInKeyOrdersAccessors(walker, walker.typeOf(object), access.key());
    LayoutID heir = variable || isStore ? LayoutID() : findClassOfReceiverInKeyOrdersAccessors(walker, walker.typeOf(object), access.key());
    if (heir)
        variable = findVariableOfInheritedStaticField(walker, heir, access.key());
    if (!variable)
        return false;

    const BodyIndex& index = walker.currentBodyIndex();
    if (heir && isStoredOffSameHolder(index, access)) {
        walker.count(StatisticID::InheritedStaticsReadAtSites);
        return false;
    }
    BodyBlock block = index.block(index.blockOf(access.id()));
    uint32_t place = access.id().index() - block.firstValue().index();
    Origin origin = walker.originOf(access);
    origin.isTailCall = false;

    constexpr Walker::IterationPurity purity = Walker::IterationPurity::KnownEffectsOnly;
    auto buildTestOfHeir = [&] {
        MemberConditionRecord isInherited { MemberConditionKind::MemberUnchanged, heir, ClassSide::Static, access.key() };
        Draft test(Opcode::AreMembersUnchanged, origin);
        test.setConditions(std::span { &isInherited, 1 });
        return walker.build(WTF::move(test));
    };
    bool testsHeir = !!heir;
    if (heir && walker.iterationsKeepStaticFieldsInVariables() && walker.iterationsPreserveMemberGuards(purity) && walker.isInvariantInSpecializedVersion(object, purity)) {
        Reference inherits;
        {
            Walker::SpecializedPreheaderScope preheaderScope(walker, true, purity);
            inherits = buildTestOfHeir();
        }
        if (!inherits || walker.hasFailed())
            return true;
        testsHeir = !walker.tryBranchInPreheader(inherits, true, purity, Walker::GuardedIterations::SomeWithUnguardedVersion);
        if (!testsHeir) {
            walker.recordSpeculation();
            walker.count(StatisticID::HeirGuardsHoisted);
        }
    }

    if (Walker::StaticFieldSeenInVariable* seen = heir ? nullptr : walker.staticFieldSeenInVariableByArm(); seen && seen->variable == variable && fieldStillLiesWhereArmHasSeenIt(walker, *seen)) {
        walker.count(StatisticID::StaticFieldGuardsReused);
        if (!isStore) {
            Reference held = seen->held;
            if (Reference asArmKnowsIt = seen->readThatNamesIt && seen->inlineFrameOfRead == origin.inlineFrame ? walker.referenceOf(seen->readThatNamesIt) : Reference())
                held = asArmKnowsIt;
            walker.bind(access.id(), held);
            *seen = { variable, held, walker.currentBlock(), walker.code().numberOfValues(), access.id(), origin.inlineFrame };
            return true;
        }
        storeWhereArmHasSeenStaticField(walker, object, variable, access.key(), seen->held, stored, origin);
        *seen = { variable, stored, walker.currentBlock(), walker.code().numberOfValues(), { }, 0 };
        return true;
    }

    if (std::optional<uint64_t> known = testsHeir ? std::nullopt : isStore ? findKnown(walker, KnownNote::StaticFieldInVariable, { }, { }, variable.index()) : findStaticFieldValue(walker, variable)) {
        walker.count(StatisticID::StaticFieldGuardsReused);
        if (isStore)
            storeStaticFieldInVariable(walker, object, variable, access.key(), stored, origin);
        else
            walker.bind(access.id(), Reference::fromBits(static_cast<uint32_t>(*known)));
        return true;
    }

    if (!testsHeir && walker.iterationsKeepStaticFieldsInVariables() && walker.isInvariantInSpecializedVersion(object, purity)) {
        Reference before;
        Reference isAbsentBefore;
        {
            Walker::SpecializedPreheaderScope preheaderScope(walker, true, purity);
            Draft read(Opcode::GetStaticFieldOrAbsent, origin);
            read.setVariable(variable);
            read.setKey(access.key());
            read.setValueType(types.withPrimitives(TypeTable::anything, Primitive::Absent));
            read.setClassObjectOperand(object);
            before = walker.build(WTF::move(read));
            if (!before || walker.hasFailed())
                return true;
            Draft test(Opcode::TypeTest, origin);
            test.setValueOperand(before);
            test.setTestedType(TypeArena::fromPrimitives(Primitive::Absent));
            isAbsentBefore = walker.build(WTF::move(test));
        }
        if (!isAbsentBefore || walker.hasFailed())
            return true;
        if (walker.tryBranchInPreheader(isAbsentBefore, false, purity, Walker::GuardedIterations::SomeWithUnguardedVersion)) {
            walker.recordSpeculation();
            walker.count(StatisticID::StaticFieldGuardsHoisted);
            if (isStore) {
                storeStaticFieldInVariable(walker, object, variable, access.key(), stored, origin);
                return true;
            }
            Reference held;
            if (walker.iterationsStoreUnderKey(access.key())) {
                held = walker.readCarriedStaticField(object, variable, access.key(), before, isAbsentBefore, origin);
                if (!held && !walker.hasFailed() && !walker.isUnreachable())
                    held = walker.readInitializedStaticField(object, variable, access.key(), origin);
            } else {
                Walker::SpecializedPreheaderScope preheaderScope(walker, true, purity);
                Draft proof(Opcode::Narrowed, origin);
                proof.setValueOperand(before);
                proof.setTestOperand(isAbsentBefore);
                proof.setOperandIndex(OperandIndex::TypeTest::value);
                proof.setIsOnTakenEdge(false);
                held = walker.build(WTF::move(proof));
            }
            noteStaticFieldInVariable(walker, variable, access.key(), held);
            walker.bind(access.id(), held);
            if (walker.hasFailed() || walker.isUnreachable() || !walker.canRestartIteration())
                return true;
            TypeID expected = findExpectation(walker, access.id()).type;
            if (expected && types.at(expected).isNumber() && !(types.at(expected).primitives & Primitive::FractionalNumber) && isCounted(index, block, access, block.numberOfValues()))
                guardExpectedType(walker, access.id(), TypeTable::int32, origin, Walker::GuardedIterations::SomeWithUnguardedVersion);
            return true;
        }
    }

    BlockID foundElsewhere;
    if (testsHeir) {
        Reference inherits = buildTestOfHeir();
        if (!inherits || walker.hasFailed())
            return true;
        foundElsewhere = walker.addBlock(BlockFrequency::Rare);
        BlockID isFoundAbove = walker.addBlock();
        walker.branch(inherits, isFoundAbove, foundElsewhere);
        walker.continueIn(isFoundAbove);
    }
    Draft read(Opcode::GetStaticFieldOrAbsent, origin);
    read.setVariable(variable);
    read.setKey(access.key());
    read.setValueType(types.withPrimitives(TypeTable::anything, Primitive::Absent));
    read.setClassObjectOperand(object);
    Reference held = walker.build(WTF::move(read));
    if (!held || walker.hasFailed())
        return true;
    bool mayRestartIteration = !isStore && !testsHeir && walker.canRestartIteration();
    Reference isAbsent;
    {
        Walker::SpecializedPreheaderScope preheaderScope(walker, mayRestartIteration && walker.isInvariantInSpecializedVersion(held));
        Draft test(Opcode::TypeTest, origin);
        test.setValueOperand(held);
        test.setTestedType(TypeArena::fromPrimitives(Primitive::Absent));
        isAbsent = walker.build(WTF::move(test));
    }
    if (!isAbsent || walker.hasFailed())
        return true;
    if (mayRestartIteration && walker.tryBranchInPreheader(isAbsent, false, Walker::IterationPurity::EffectFree, Walker::GuardedIterations::SomeWithUnguardedVersion)) {
        Walker::SpecializedPreheaderScope preheaderScope(walker, true);
        Draft proof(Opcode::Narrowed, origin);
        proof.setValueOperand(held);
        proof.setTestOperand(isAbsent);
        proof.setOperandIndex(OperandIndex::TypeTest::value);
        proof.setIsOnTakenEdge(false);
        walker.recordSpeculation();
        walker.bind(access.id(), walker.build(WTF::move(proof)));
        walker.count(StatisticID::StaticFieldGuardsHoisted);
        return true;
    }

    uint32_t end = place + 1;
    static constexpr OpcodeSet operationsBetweenAccessesOfOneLook = constantsInFormOfUses | inputIdentityOpcodes | allNumericOperators | allRelations | allEqualities | OpcodeSet { Opcode::GetModuleVariable, Opcode::CheckInitialized };
    bool mayTakeMore = walker.isInLoop() && !walker.staticFieldSeenInVariableByArm() && !walker.isNestedTooDeeply();
    for (uint32_t later = end, limit = mayTakeMore ? limitOfStretchOfArms(walker, block) : 0; later < limit; ++later) {
        BodyValue other = index.value(ValueID(block.firstValue().index() + later));
        if ((other.opcode() == Opcode::PropGet || other.opcode() == Opcode::PropSet) && !other.isFreeName() && other.key() == access.key() && namesSameHolderInKeyOrdersAccessors(index, other.objectOperand(), access.objectOperand()))
            end = later + 1;
        else if (!operationsBetweenAccessesOfOneLook.contains(other.opcode()))
            break;
    }

    BlockID asWritten = foundElsewhere ? foundElsewhere : walker.addBlock(BlockFrequency::Rare);
    BlockID liesInVariable = walker.addBlock();
    BlockID meeting = walker.addBlock();
    walker.recordSpeculation();
    walker.noteStaticFieldInTries();
    walker.branch(isAbsent, asWritten, liesInVariable);

    Walker::StretchInArms stretch = walker.stretchInArms(block.id(), place, end);
    Walker::ArmsThatMeet arms;

    walker.continueIn(liesInVariable);
    if (!walker.isUnreachable()) {
        Draft proof(Opcode::Narrowed, origin);
        proof.setValueOperand(held);
        proof.setTestOperand(isAbsent);
        proof.setOperandIndex(OperandIndex::TypeTest::value);
        proof.setIsOnTakenEdge(false);
        Reference liesThere = !isStore || walker.canRecordUndoableStore() ? walker.build(WTF::move(proof)) : Reference();
        if (isStore) {
            storeWhereArmHasSeenStaticField(walker, object, variable, access.key(), liesThere, stored, origin);
            liesThere = stored;
        } else
            walker.bind(access.id(), liesThere);
        if (end > place + 1 && liesThere && !walker.hasFailed() && !walker.isUnreachable()) {
            Walker::StaticFieldSeenInVariable seen { variable, liesThere, liesInVariable, walker.code().numberOfValues(), isStore ? ValueID() : access.id(), origin.inlineFrame };
            SetForScope ofThisArm(walker.staticFieldSeenInVariableByArm(), &seen);
            TypeID expected = isStore ? TypeID() : findExpectation(walker, access.id()).type;
            if (expected && types.at(expected).isNumber() && !(types.at(expected).primitives & Primitive::FractionalNumber) && isCounted(index, block, access, end))
                expected = TypeTable::int32;
            if (expected)
                openGuess(walker, liesThere, expected, block.id(), place + 1, end);
            else
                walker.walkStretch(block.id(), place + 1, end);
        }
        walker.listArm(stretch, arms);
    }
    if (walker.hasFailed())
        return true;

    walker.walkClosedArm(asWritten, stretch, arms);
    if (!walker.hasFailed())
        walker.joinArms(stretch, arms, meeting);
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
