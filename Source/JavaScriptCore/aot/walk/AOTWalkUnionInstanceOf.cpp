/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTBuiltinMembers.h"
#include "AOTBuiltinPristine.h"
#include "AOTBuiltinTableBindings.h"
#include "AOTBuiltinTableRealm.h"
#include "AOTBuiltinTypes.h"
#include "AOTFactRecords.h"
#include "AOTIRProgram.h"
#include "AOTLayout.h"
#include "AOTLinkTables.h"
#include "AOTTypeArena.h"
#include "AOTTypeRelations.h"
#include <algorithm>
#include <optional>
#include <span>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

namespace {

using ConditionsOfInstanceOf = Vector<MemberConditionRecord, 4>;

enum class ChainEndForInstanceOf : uint8_t { Unknown, MeetsClass, MissesClass };

struct ChainsForInstanceOf {
    Vector<LayoutID, 8> decided;
    Vector<LayoutID, 8> instances;
    bool testsObjects { false };
    bool hasUndecidedLayouts { false };
};

struct ClassTestedByInstanceOf {
    LayoutID layout;
    bool isConstructorFunction { false };
    ConditionsOfInstanceOf conditions;
};

struct FormOfInstanceOf {
    LayoutID tested;
    bool testedIsConstructorFunction { false };
    ConditionsOfInstanceOf conditions;
    LayoutSetID instances;
    bool missMeansFalse { false };
    bool areAllInstances { false };
    bool valueIsSurelyObject { false };
    bool testsPrototypeOfInstance { false };
};


struct BuiltinTestedByInstanceOf {
    BuiltinID constructor;
    BuiltinID prototype;
    BuiltinClassID builtinClass;
};

class AnswersOfInstanceOf {
public:
    explicit AnswersOfInstanceOf(Walker& walker)
        : m_walker(walker)
    {
    }

    void arrive(Reference answer)
    {
        if (!answer || m_walker.isUnreachable())
            return;
        if (!m_meeting) {
            m_meeting = m_walker.addBlock();
            m_answer = m_walker.addParameter(m_meeting, TypeTable::boolean, m_walker.formOfParameter(TypeTable::boolean, { }));
        }
        m_walker.jumpTo(m_meeting, singleElementSpan(answer));
    }

    Reference meet()
    {
        if (!m_meeting)
            return { };
        m_walker.continueIn(m_meeting);
        return m_walker.isUnreachable() ? Reference() : m_answer;
    }

private:
    Walker& m_walker;
    BlockID m_meeting;
    Reference m_answer;
};

} // anonymous namespace

static void addConditionOfInstanceOf(ConditionsOfInstanceOf& conditions, const MemberConditionRecord& condition)
{
    if (!conditions.contains(condition))
        conditions.append(condition);
}

static OptionSet<LayoutChange> possibleChangesForInstanceOf(const LinkTables& link, LayoutID layout)
{
    return layout.index() < link.layouts.size() ? link.layout(layout).possibleChanges : LinkedLayout { }.possibleChanges;
}

static bool classStaysAsWrittenForInstanceOf(const LinkTables& link, LayoutID classLayout)
{
    return !possibleChangesForbid(possibleChangesForInstanceOf(link, classLayout), TypedAccess::FindsMethodAbove);
}

static bool mayBuildArmsForInstanceOf(const Walker& walker)
{
    if (!walker.has(Mechanism::Guesses) || !walker.opensGuesses())
        return false;
    if (walker.isInLoop())
        return true;
    FunctionID function = walker.currentFunction();
    return !walker.facts().runsAtMostOnce(function) && !walker.program().at(function).isModuleBody();
}

static Reference booleanAnswerOfInstanceOf(Walker& walker, bool answer)
{
    Draft constant(Opcode::Boolean);
    constant.setBoolean(answer);
    return walker.constant(WTF::move(constant));
}

static Reference buildInstanceOfAsWritten(Walker& walker, const BodyValue& test)
{
    Draft written = walker.draftOf(test);
    walker.chooseConservativeFields(written);
    return walker.build(WTF::move(written));
}

static Reference buildProofOfInstanceOf(Walker& walker, Reference value, Reference test, uint32_t operandIndex, Origin origin)
{
    Draft proof(Opcode::Narrowed, origin);
    walker.chooseConservativeFields(proof);
    proof.setValueOperand(value);
    proof.setTestOperand(test);
    proof.setOperandIndex(operandIndex);
    proof.setIsOnTakenEdge(true);
    return walker.build(WTF::move(proof));
}

static Reference buildTypeTestOfInstanceOf(Walker& walker, Reference value, TypeID tested, Origin origin)
{
    Draft test(Opcode::TypeTest, origin);
    walker.chooseConservativeFields(test);
    test.setValueOperand(value);
    test.setTestedType(tested);
    return walker.build(WTF::move(test));
}

static Reference buildWalkOfChainForInstanceOf(Walker& walker, Reference value, Reference prototype, Origin origin)
{
    if (!prototype)
        return { };
    Draft chainWalk(Opcode::HasInPrototypeChain, origin);
    walker.chooseConservativeFields(chainWalk);
    chainWalk.setValueOperand(value);
    chainWalk.setPrototypeOperand(prototype);
    return walker.build(WTF::move(chainWalk));
}

static bool hasOneClassObjectForInstanceOf(const Walker& walker, LayoutID classLayout)
{
    if (!walker.facts().isEvaluatedAtMostOnce(classLayout))
        return false;
    const Layout& layout = walker.program().at(classLayout);
    if (!layout.has(LayoutFlags::IsConstructorFunction))
        return true;
    return layout.constructor && walker.program().at(layout.constructor).captures.size() == walker.plan().promotedEnvironments().numberOfRemovedCapturesOf(layout.constructor);
}

static bool findsOrdinaryHasInstanceForInstanceOf(const Walker& walker, const ClassTestedByInstanceOf& tested, ConditionsOfInstanceOf& conditions)
{
    const Program& program = walker.program();
    const BuiltinTable& table = program.builtins();
    const LinkTables& link = walker.facts().linked();
    PropertyKeyID key = program.keyOf(table.nameOf(WellKnownSymbol::HasInstance));
    BuiltinID ordinary = findConstantMemberFrom(program, walker.facts(), Builtins::FunctionPrototype, key);
    if (!ordinary || table.canonicalID(ordinary) != Builtins::FunctionPrototypeSymbolHasInstance)
        return false;

    if (!tested.isConstructorFunction && classStaysAsWrittenForInstanceOf(link, tested.layout) && link.findMember(program, tested.layout, ClassSide::Static, key).kind == MemberFound::Kind::Nothing) {
        bool mayBeHidden = false;
        size_t stepsLeft = program.layouts.size();
        for (LayoutID current = tested.layout; current && stepsLeft; current = program.at(current).base, --stepsLeft) {
            for (const StaticField& field : program.at(current).staticFields)
                mayBeHidden |= !field.key || field.key == key;
        }
        if (!mayBeHidden && stepsLeft)
            return true;
    }

    if (FunctionID constructor = program.at(tested.layout).constructor; tested.isConstructorFunction && constructor && link.closuresLackName(constructor, key))
        return true;

    UNUSED_PARAM(conditions);
    return false;
}

static std::optional<ClassTestedByInstanceOf> describeClassTestedByInstanceOf(const Walker& walker, TypeID typeOfConstructor)
{
    const Program& program = walker.program();
    TypeArena& types = walker.types();
    Type type = types.at(typeOfConstructor);
    FunctionID constructor = type.isObject() && type.isJavaScriptValue() ? singleFunctionOf(types, type) : FunctionID();
    if (!constructor)
        return std::nullopt;

    ClassTestedByInstanceOf tested;
    tested.layout = classOfClassObject(program, constructor);
    if (!tested.layout) {
        tested.layout = classOfConstructorFunction(program, constructor);
        tested.isConstructorFunction = true;
    }
    if (!tested.layout || program.at(tested.layout).kind != LayoutKind::Instance)
        return std::nullopt;

    bool isEvaluatedOnce = hasOneClassObjectForInstanceOf(walker, tested.layout);
    if (tested.isConstructorFunction && !walker.facts().isEvaluatedAtMostOnce(tested.layout))
        return std::nullopt;
    if (!findsOrdinaryHasInstanceForInstanceOf(walker, tested, tested.conditions))
        return std::nullopt;

    if (tested.isConstructorFunction) {
        tested.conditions.removeAllMatching([&](const MemberConditionRecord& condition) {
            return condition.kind == MemberConditionKind::PrototypeLinkUnchanged && condition.layout == tested.layout && condition.side == ClassSide::Static;
        });
        if (!walker.facts().linked().layouts[tested.layout.index()].prototypeObjectIsSettledAheadOfEveryObject)
            addConditionOfInstanceOf(tested.conditions, { MemberConditionKind::MemberUnchanged, tested.layout, ClassSide::Static, program.keyOf(BuiltinNames::prototype) });
    }
    if (!walker.facts().isEvaluatedAtMostOnce(tested.layout))
        addConditionOfInstanceOf(tested.conditions, { MemberConditionKind::ClassEvaluatedOnce, tested.layout, ClassSide::Prototype, { } });
    if (!tested.conditions.isEmpty() && (!walker.plan().hasClassRecord(tested.layout) || (tested.isConstructorFunction && !isEvaluatedOnce)))
        return std::nullopt;
    return tested;
}

static ChainEndForInstanceOf followChainForInstanceOf(const Walker& walker, LayoutID layout, LayoutID tested)
{
    const Program& program = walker.program();
    const LinkTables& link = walker.facts().linked();
    const Layout& start = program.at(layout);
    if (start.has(LayoutFlags::IsOfCreatedObject))
        return ChainEndForInstanceOf::Unknown;
    if (start.kind == LayoutKind::Record)
        return !start.constructor && !start.has(LayoutFlags::IsConstructorFunction) ? ChainEndForInstanceOf::MissesClass : ChainEndForInstanceOf::Unknown;
    if (start.kind != LayoutKind::Instance)
        return ChainEndForInstanceOf::Unknown;

    LayoutID current = declaredLayoutOf(program, layout);
    for (size_t unvisitedClasses = program.layouts.size(); current && unvisitedClasses; --unvisitedClasses) {
        if (current == tested)
            return ChainEndForInstanceOf::MeetsClass;
        const Layout& passed = program.at(current);
        if (passed.kind != LayoutKind::Instance || !passed.constructor)
            return ChainEndForInstanceOf::Unknown;
        if (passed.has(LayoutFlags::IsConstructorFunction)) {
            bool standsBelowObjectPrototype = !passed.base && link.layouts[current.index()].prototypeObjectIsSettledAheadOfEveryObject;
            return standsBelowObjectPrototype ? ChainEndForInstanceOf::MissesClass : ChainEndForInstanceOf::Unknown;
        }

        LayoutID next = classProvenAbove(program, link, current);
        if (!next)
            return classStaysAsWrittenForInstanceOf(link, current) && !passed.has(LayoutFlags::HasHeritage) ? ChainEndForInstanceOf::MissesClass : ChainEndForInstanceOf::Unknown;
        if (program.at(next).has(LayoutFlags::IsConstructorFunction))
            return ChainEndForInstanceOf::Unknown;
        current = next;
    }
    return ChainEndForInstanceOf::Unknown;
}

static ChainsForInstanceOf followChainsForInstanceOf(const Walker& walker, std::span<const LayoutID> ascendingLayouts, LayoutID tested)
{
    const Program& program = walker.program();
    const LinkTables& link = walker.facts().linked();
    bool prototypeObjectMayBeShared = program.at(tested).has(LayoutFlags::IsConstructorFunction) && !link.layouts[tested.index()].prototypeObjectIsSettledAheadOfEveryObject;
    ChainsForInstanceOf chains;
    for (LayoutID layout : ascendingLayouts) {
        if (!walker.plan().isInUse(layout))
            continue;

        ChainEndForInstanceOf end = followChainForInstanceOf(walker, layout, tested);
        bool isMadeByConstructorFunction = program.at(layout).kind == LayoutKind::Instance && program.at(declaredLayoutOf(program, layout)).has(LayoutFlags::IsConstructorFunction)
            && !link.layouts[declaredLayoutOf(program, layout).index()].prototypeObjectIsSettledAheadOfEveryObject;
        bool mayGetOtherPrototype = possibleChangesForInstanceOf(link, layout).contains(LayoutChange::ChangesPrototype);
        bool testsObject = false;
        if (end == ChainEndForInstanceOf::MissesClass && (mayGetOtherPrototype || prototypeObjectMayBeShared))
            end = ChainEndForInstanceOf::Unknown;
        if (end == ChainEndForInstanceOf::MeetsClass && (isMadeByConstructorFunction || mayGetOtherPrototype)) {
            if (isFoundByShapeTest(program.at(layout)))
                testsObject = true;
            else
                end = ChainEndForInstanceOf::Unknown;
        }
        if (end == ChainEndForInstanceOf::Unknown) {
            chains.hasUndecidedLayouts = true;
            continue;
        }

        chains.decided.append(layout);
        if (end == ChainEndForInstanceOf::MeetsClass)
            chains.instances.append(layout);
        chains.testsObjects |= testsObject;
    }
    return chains;
}

static bool programMakesNoProxyForInstanceOf(const Walker& walker)
{
    const Facts& facts = walker.facts();
    if (facts.unseenCodeCanRun())
        return false;
    auto isInReach = [&](BuiltinID builtin) {
        return !!findFactRow(facts.builtinUses(), builtin.index(), &BuiltinUseRecord::builtin);
    };
    return !isInReach(Builtins::Proxy) && !isInReach(Builtins::ProxyRevocable);
}

static bool isTestableByShapeForInstanceOf(const Walker& walker, std::span<const LayoutID> layouts)
{
    return std::ranges::all_of(layouts, [&](LayoutID layout) {
        return isFoundByShapeTest(walker.program().at(layout)) && walker.plan().slots().holdsCompiledObjects(layout) && !walker.plan().shapes().shapesOf(layout).isEmpty();
    });
}

static std::optional<FormOfInstanceOf> planFormOfInstanceOf(const Walker& walker, TypeID typeOfValue, TypeID typeOfConstructor)
{
    if (!walker.plan().hasOneInstance())
        return std::nullopt;
    std::optional<ClassTestedByInstanceOf> tested = describeClassTestedByInstanceOf(walker, typeOfConstructor);
    if (!tested)
        return std::nullopt;
    const Program& program = walker.program();
    TypeArena& types = walker.types();
    Type value = types.at(typeOfValue);
    if (!value.isJavaScriptValue() || value.mayBeUnspecified())
        return std::nullopt;

    Vector<LayoutID, 8> considered;
    bool areAllConsidered = !value.hasObjects();
    if (value.hasObjects()) {
        Type objects = types.at(types.withOnlyObjects(typeOfValue));
        areAllConsidered = objects.hasOnlyLayouts() && !objects.traitsOfNamed.contains(ObjectTrait::MayHaveLayoutBelow);
        if (areAllConsidered)
            considered.append(membersOf(types, objects.layouts));
        else {
            considered.append(tested->layout);
            considered.append(program.layoutsExpectedBelow(tested->layout));
            std::ranges::sort(considered);
        }
    }

    ChainsForInstanceOf chains = followChainsForInstanceOf(walker, considered.span(), tested->layout);
    FormOfInstanceOf form;
    form.tested = tested->layout;
    form.testedIsConstructorFunction = tested->isConstructorFunction;
    form.conditions = WTF::move(tested->conditions);
    form.valueIsSurelyObject = value.isObject();
    form.testsPrototypeOfInstance = chains.testsObjects;

    const LinkTables& link = walker.facts().linked();
    bool onlyInstancesStandBelow = !form.testedIsConstructorFunction && form.tested.index() < link.layouts.size() && link.layout(form.tested).onlyOwnObjectsStandBelowPrototype && programMakesNoProxyForInstanceOf(walker);
    form.missMeansFalse = !chains.hasUndecidedLayouts && (areAllConsidered || onlyInstancesStandBelow);
    form.areAllInstances = areAllConsidered && form.missMeansFalse && !chains.decided.isEmpty() && chains.instances.size() == chains.decided.size();

    if (chains.instances.isEmpty())
        return form;
    if (isTestableByShapeForInstanceOf(walker, chains.instances.span()))
        form.instances = types.layoutSets().ofSorted(chains.instances.span());
    else {
        form.missMeansFalse = false;
        form.areAllInstances = false;
    }
    return form;
}

static bool buildInstanceOfClassOfProgram(Walker& walker, const BodyValue& test, const InputsOfTypedOperation& inputs)
{
    TypeID typeOfValue = inputs.typeOf(test.valueOperand());
    TypeID typeOfConstructor = inputs.typeOf(test.constructorOperand());
    if (!typeOfValue || !typeOfConstructor)
        return false;
    std::optional<FormOfInstanceOf> form = planFormOfInstanceOf(walker, typeOfValue, typeOfConstructor);
    if (!form || !form->conditions.isEmpty())
        return false;

    bool isConstant = !form->instances ? form->missMeansFalse : form->areAllInstances && form->valueIsSurelyObject && !form->testsPrototypeOfInstance;
    if (isConstant && !walker.has(Mechanism::Folding)) {
        form->instances = { };
        form->missMeansFalse = false;
        form->areAllInstances = false;
    }
    bool answersWithoutArms = form->conditions.isEmpty() && (!form->instances || (form->missMeansFalse && !form->testsPrototypeOfInstance));
    if (!answersWithoutArms && !mayBuildArmsForInstanceOf(walker)) {
        if (!form->conditions.isEmpty())
            return false;
        form->instances = { };
        form->missMeansFalse = false;
        form->areAllInstances = false;
    }
    if (form->testedIsConstructorFunction && !form->instances && !form->missMeansFalse)
        return false;
    if (inputs.areOnlyTypes())
        return true;

    Reference value = walker.referenceOf(test.valueOperand());
    Reference constructor = walker.referenceOf(test.constructorOperand());
    if (!value || !value.isRecord() || !constructor || !constructor.isRecord())
        return false;
    if (form->instances && !value.isValue()) {
        if (form->testedIsConstructorFunction || !form->conditions.isEmpty())
            return false;
        form->instances = { };
        form->missMeansFalse = false;
        form->areAllInstances = false;
    }

    Origin origin = walker.originOf(test);
    AnswersOfInstanceOf answers(walker);
    bool hasArms = false;

    auto buildAnswerOfMiss = [&]() -> Reference {
        if (form->missMeansFalse)
            return booleanAnswerOfInstanceOf(walker, false);
        if (form->testedIsConstructorFunction)
            return buildInstanceOfAsWritten(walker, test);
        Draft read(Opcode::GetClassPrototype, origin);
        walker.chooseConservativeFields(read);
        read.setClassObjectOperand(constructor);
        read.setLayout(form->tested);
        return buildWalkOfChainForInstanceOf(walker, value, walker.build(WTF::move(read)), origin);
    };

    bool testsPrototype = form->testsPrototypeOfInstance && !walker.state().factsOfCell(value).contains(FactOfCell::HasLayoutPrototype);
    Reference answer;
    if (!form->instances)
        answer = buildAnswerOfMiss();
    else if (form->areAllInstances && form->valueIsSurelyObject && !testsPrototype && walker.has(Mechanism::Folding))
        answer = booleanAnswerOfInstanceOf(walker, true);
    else {
        Draft shapeTest(Opcode::ShapeTest, origin);
        walker.chooseConservativeFields(shapeTest);
        shapeTest.setObjectOperand(value);
        shapeTest.setLayouts(form->instances);
        if (!hasArms && form->missMeansFalse && !testsPrototype)
            shapeTest.inputValueGiven = test.id();
        Reference isInstance = walker.build(WTF::move(shapeTest));
        if (!isInstance)
            return true;
        if (form->missMeansFalse && !testsPrototype)
            answer = isInstance;
        else {
            BlockID hasShape = walker.addBlock();
            BlockID walksChain = walker.addBlock(BlockFrequency::Rare);
            BlockID hasOtherShape = form->missMeansFalse ? walker.addBlock() : walksChain;
            walker.branch(isInstance, hasShape, hasOtherShape);
            if (!hasArms)
                walker.noteGuessOpened();
            hasArms = true;

            walker.continueIn(hasShape);
            if (testsPrototype) {
                Reference instance = buildProofOfInstanceOf(walker, value, isInstance, OperandIndex::ShapeTest::object, origin);
                if (!instance)
                    return true;
                Draft linkTest(Opcode::HasLayoutPrototype, origin);
                walker.chooseConservativeFields(linkTest);
                linkTest.setObjectOperand(instance);
                Reference standsWhereMade = walker.build(WTF::move(linkTest));
                if (!standsWhereMade)
                    return true;
                BlockID isBelowClass = walker.addBlock();
                walker.branch(standsWhereMade, isBelowClass, walksChain);
                walker.continueIn(isBelowClass);
            }
            answers.arrive(booleanAnswerOfInstanceOf(walker, true));

            if (form->missMeansFalse) {
                walker.continueIn(hasOtherShape);
                answers.arrive(booleanAnswerOfInstanceOf(walker, false));
            }
            if (!form->missMeansFalse || testsPrototype) {
                walker.continueIn(walksChain);
                bool missMeansFalse = std::exchange(form->missMeansFalse, false);
                answers.arrive(buildAnswerOfMiss());
                form->missMeansFalse = missMeansFalse;
            }
        }
    }

    if (hasArms) {
        answers.arrive(answer);
        answer = answers.meet();
    }
    if (answer)
        walker.bind(test.id(), answer);
    return true;
}

static std::optional<BuiltinTestedByInstanceOf> describeBuiltinTestedByInstanceOf(const Walker& walker, TypeID typeOfConstructor)
{
    const Program& program = walker.program();
    const BuiltinTable& table = program.builtins();
    Type written = walker.types().at(typeOfConstructor);
    BuiltinID constructor = written.isObject() ? singleBuiltinOf(walker.types(), written) : BuiltinID();
    if (!constructor || table.at(constructor).kind != BuiltinKind::Constructor)
        return std::nullopt;

    BuiltinTestedByInstanceOf tested;
    tested.constructor = table.canonicalID(constructor);
    if (tested.constructor == table.canonicalID(Builtins::File))
        return std::nullopt;
    BuiltinID asked = findConstantMemberFrom(program, walker.facts(), constructor, program.keyOf(table.nameOf(WellKnownSymbol::HasInstance)));
    if (!asked || table.canonicalID(asked) != Builtins::FunctionPrototypeSymbolHasInstance)
        return std::nullopt;

    BuiltinID location = findConstantMemberFrom(program, walker.facts(), constructor, program.keyOf(BuiltinNames::prototype));
    BuiltinID prototype = location ? table.canonicalID(location) : BuiltinID();
    if (!prototype || !isObjectKind(table.at(prototype).kind) || !BuiltinCellKey::object(prototype).hasCell(table) || isMadeWithCodeOfLibrary(table, location))
        return tested;
    tested.prototype = prototype;

    BuiltinClassID builtinClass = table.at(tested.constructor).instanceClass;
    if (!builtinClass)
        return tested;
    const BuiltinClass& instanceClass = table.at(builtinClass);
    if (instanceClass.constructor && instanceClass.prototype && table.canonicalID(instanceClass.constructor) == tested.constructor && table.canonicalID(instanceClass.prototype) == prototype)
        tested.builtinClass = builtinClass;
    return tested;
}

static bool holdsNoObjectForInstanceOf(const Walker& walker, TypeID typeOfValue)
{
    Type subject = walker.types().at(typeOfValue);
    return subject.isJavaScriptValue() && !subject.mayBeUnspecified() && !subject.hasObjects();
}

static bool classTestAnswersBuiltinInstanceOf(const Walker& walker, const BuiltinTestedByInstanceOf& tested, TypeID typeOfValue)
{
    const BuiltinTable& table = walker.program().builtins();
    if (!tested.builtinClass || !walker.has(Mechanism::EscapeAnalysis) || !walker.plan().hasOneInstance())
        return false;
    if (pristineTestOf(table, tested.builtinClass) != PristineTest::MayPass || !table.mayHavePristineInstances(tested.builtinClass))
        return false;
    TypeArena& types = walker.types();
    TypeID instances = types.builtinClass(tested.builtinClass);
    if (!types.overlap(typeOfValue, instances))
        return false;
    return (hasInstanceTest(table, tested.builtinClass) && types.testablePartOf(instances) == instances) || types.isSubset(typeOfValue, instances);
}

static Reference buildWalkOfChainToBuiltinForInstanceOf(Walker& walker, Reference value, BuiltinID prototype, Origin origin)
{
    Draft found(Opcode::Builtin, origin);
    walker.chooseConservativeFields(found);
    found.setBuiltin(prototype);
    return buildWalkOfChainForInstanceOf(walker, value, walker.build(WTF::move(found)), origin);
}

static Reference buildClassTestsOfBuiltinInstanceOf(Walker& walker, const BuiltinTestedByInstanceOf& tested, Reference value, Origin origin)
{
    TypeArena& types = walker.types();
    TypeID instances = types.builtinClass(tested.builtinClass);
    AnswersOfInstanceOf answers(walker);
    BlockID walksChain = walker.addBlock(BlockFrequency::Unlikely);
    walker.noteGuessOpened();

    Reference cell = value;
    if (!types.isSubset(walker.typeOf(value), instances)) {
        Reference isOfClass = buildTypeTestOfInstanceOf(walker, value, instances, origin);
        if (!isOfClass)
            return { };
        BlockID ofClass = walker.addBlock();
        walker.branch(isOfClass, ofClass, walksChain);
        walker.continueIn(ofClass);
        cell = buildProofOfInstanceOf(walker, value, isOfClass, OperandIndex::TypeTest::value, origin);
        if (!cell)
            return { };
    }
    Reference isAsMade = buildTestOfObjectAsMade(walker, cell, origin);
    if (isAsMade) {
        BlockID asMade = walker.addBlock();
        walker.branch(isAsMade, asMade, walksChain);
        walker.continueIn(asMade);
        answers.arrive(booleanAnswerOfInstanceOf(walker, true));
    } else if (!walker.isUnreachable())
        walker.jumpTo(walksChain);

    walker.continueIn(walksChain);
    answers.arrive(buildWalkOfChainToBuiltinForInstanceOf(walker, value, tested.prototype, origin));
    return answers.meet();
}

static bool buildInstanceOfBuiltinByValue(Walker& walker, const BodyValue& test, const InputsOfTypedOperation& inputs)
{
    TypeID typeOfValue = inputs.typeOf(test.valueOperand());
    TypeID typeOfConstructor = inputs.typeOf(test.constructorOperand());
    if (!typeOfValue || !typeOfConstructor)
        return false;
    std::optional<BuiltinTestedByInstanceOf> tested = describeBuiltinTestedByInstanceOf(walker, typeOfConstructor);
    if (!tested)
        return false;

    bool holdsNoObject = walker.has(Mechanism::Folding) && holdsNoObjectForInstanceOf(walker, typeOfValue);
    bool testsClass = false;
    if (!holdsNoObject && tested->prototype && mayBuildArmsForInstanceOf(walker) && classTestAnswersBuiltinInstanceOf(walker, *tested, typeOfValue)) {
        TypeID expected = walker.isInLoop() ? TypeID() : findExpectation(walker, test.valueOperand()).type;
        testsClass = walker.isInLoop() || (expected && walker.types().overlap(expected, walker.types().builtinClass(tested->builtinClass)));
    }
    if (inputs.areOnlyTypes())
        return holdsNoObject || testsClass;

    Reference value = walker.referenceOf(test.valueOperand());
    Reference constructor = walker.referenceOf(test.constructorOperand());
    if (!value || !value.isRecord() || !constructor || !constructor.isRecord())
        return false;
    Origin origin = walker.originOf(test);
    Reference answer;
    if (holdsNoObject)
        answer = booleanAnswerOfInstanceOf(walker, false);
    else {
        bool isSeenAsMade = tested->builtinClass && walker.has(Mechanism::Folding) && walker.plan().hasOneInstance() && walker.types().isSubset(walker.typeOf(value), walker.types().builtinClass(tested->builtinClass)) && isListAsMade(walker, value);
        if (isSeenAsMade)
            answer = booleanAnswerOfInstanceOf(walker, true);
        else if (testsClass && value.isValue())
            answer = buildClassTestsOfBuiltinInstanceOf(walker, *tested, value, origin);
        else
            return false;
    }
    if (answer)
        walker.bind(test.id(), answer);
    return true;
}

static_assert(opcodesThatBuildFormOnShapesTakes == (OpcodeSet { Opcode::InstanceOf, Opcode::RequireObjectCoercible }));

bool buildFormOnShapesOfItsOpcodes(Walker& walker, const BodyValue& value, const InputsOfTypedOperation& inputs)
{
    switch (value.opcode()) {
    case Opcode::InstanceOf:
        return buildInstanceOfClassOfProgram(walker, value, inputs) || buildInstanceOfBuiltinByValue(walker, value, inputs);
    case Opcode::RequireObjectCoercible:
        return buildCheckThatTypesPass(walker, value, inputs);
    default:
        return false;
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
