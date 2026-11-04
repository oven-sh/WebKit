/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// `x instanceof Leaf`, `x instanceof Map`: both lookups on the right side are answered, and the left side is asked for its shape or the kind of its cell. Whatever that does not decide walks the chain, or is the operation as written.
// At a suspension: nothing. At a throw: only the operation as written and the walk of the chain throw, under the handler of the block being walked.
// Withheld with TypedOperations. Whatever needs arms is withheld with Guesses, an answer that is a constant with Folding.

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
    // Both ascending.
    Vector<LayoutID, 8> decided;
    Vector<LayoutID, 8> instances;
    ConditionsOfInstanceOf conditions;
    // An object with the shape of an instance may stand below something else by now.
    bool testsObjects { false };
    bool hasUndecidedLayouts { false };
};

struct ClassTestedByInstanceOf {
    LayoutID layout;
    bool isConstructorFunction { false };
    // While these hold, the class object finds the ordinary Symbol.hasInstance and its `prototype` holds the prototype object of its class.
    ConditionsOfInstanceOf conditions;
};

struct FormOfInstanceOf {
    LayoutID tested;
    bool testedIsConstructorFunction { false };
    ConditionsOfInstanceOf conditions;
    // An object with one of these shapes is an instance, once the test of the object has passed. Null: no shape is asked for.
    LayoutSetID instances;
    // Otherwise a miss walks the chain.
    bool missMeansFalse { false };
    // Every object that the value can be has one of the shapes.
    bool areAllInstances { false };
    bool valueIsSurelyObject { false };
    bool testsPrototypeOfInstance { false };
};

enum class AnswerOfBuiltinInstanceOf : uint8_t { Unknown, No, Yes, YesIfObject };

struct BuiltinTestedByInstanceOf {
    BuiltinID constructor;
    // The object that the constructor keeps as its `prototype` whatever runs, by its canonical number. Null: only a read off the constructor tells.
    BuiltinID prototype;
    // The class whose prototype object that is. Null: there is none.
    BuiltinClassID builtinClass;
};

// The ways through a form meet in one block, which is made when the first of them comes there.
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

    // Null: no way comes through.
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

// Whoever lays another object above a prototype object or a class object, defines a property of either by a descriptor or deletes one lets the class escape, with the classes below it.
static bool classStaysAsWrittenForInstanceOf(const LinkTables& link, LayoutID classLayout)
{
    return !possibleChangesForbid(possibleChangesForInstanceOf(link, classLayout), TypedAccess::FindsMethodAbove);
}

// Arms cost bytes at the site, and gain nothing where the code does not come again.
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

// ---- A class of the program: the right side

// A constructor function whose closures hold a capture has a function object for each environment, and nothing ever sets the guards of such a function.
static bool hasOneClassObjectForInstanceOf(const Walker& walker, LayoutID classLayout)
{
    if (!walker.facts().isEvaluatedAtMostOnce(classLayout))
        return false;
    const Layout& layout = walker.program().at(classLayout);
    if (!layout.has(LayoutFlags::IsConstructorFunction))
        return true;
    return layout.constructor && walker.program().at(layout.constructor).captures.size() == walker.plan().promotedEnvironments().numberOfRemovedCapturesOf(layout.constructor);
}

// The lookup passes every class object of the row and comes to Function.prototype, whose member nobody can replace. Of a class Link may have proven that. Else the guards of the class answer, which the lookup of classes lists.
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
        // A static field is a property of the class object that declares it. One under a computed key answers to a name that nobody knows.
        bool mayBeHidden = false;
        size_t stepsLeft = program.layouts.size();
        for (LayoutID current = tested.layout; current && stepsLeft; current = program.at(current).base, --stepsLeft) {
            for (const StaticField& field : program.at(current).staticFields)
                mayBeHidden |= !field.key || field.key == key;
        }
        if (!mayBeHidden && stepsLeft)
            return true;
    }

    // No closure of the function has a name of its own but `prototype`, and each stands below Function.prototype.
    if (FunctionID constructor = program.at(tested.layout).constructor; tested.isConstructorFunction && constructor && link.closuresLackName(constructor, key))
        return true;

    // The way behind the guards of the class waits for the lookup of classes: withdrawn/ has it.
    UNUSED_PARAM(conditions);
    return false;
}

// A bound function, a proxy and whatever else is no class object of the program has no class here: the operation as written unwraps, calls and throws as the language says.
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
    if (tested.isConstructorFunction && !isEvaluatedOnce)
        return std::nullopt;
    if (!findsOrdinaryHasInstanceForInstanceOf(walker, tested, tested.conditions))
        return std::nullopt;

    if (tested.isConstructorFunction) {
        // `Object.setPrototypeOf(Readable, Stream)`: the function has another parent than Function.prototype from before its first construction on. The guard of the member speaks of the chain that is there.
        tested.conditions.removeAllMatching([&](const MemberConditionRecord& condition) {
            return condition.kind == MemberConditionKind::PrototypeLinkUnchanged && condition.layout == tested.layout && condition.side == ClassSide::Static;
        });
        // No code can change the property of a class. That of a function holds one object from before the first object of the layout on, or its guard answers.
        if (!walker.facts().linked().layouts[tested.layout.index()].prototypeObjectIsSettledAheadOfEveryObject)
            addConditionOfInstanceOf(tested.conditions, { MemberConditionKind::MemberUnchanged, tested.layout, ClassSide::Static, program.keyOf(BuiltinNames::prototype) });
    }
    // Each evaluation of a class has a prototype object of its own, and all of them share the layout.
    if (!isEvaluatedOnce)
        addConditionOfInstanceOf(tested.conditions, { MemberConditionKind::ClassEvaluatedOnce, tested.layout, ClassSide::Prototype, { } });
    // The run time ends a guard through the record of its class. Without a record the guard never holds.
    if (!tested.conditions.isEmpty() && !walker.plan().hasClassRecord(tested.layout))
        return std::nullopt;
    return tested;
}

// ---- A class of the program: the chains of the layouts that the left side may have

// parentNamedByClassRecord() of image/AOTClassRecords.cpp. False: above the class stands what no record of the program describes.
static bool findParentNamedByClassRecordForInstanceOf(const Walker& walker, LayoutID classLayout, LayoutID& parent)
{
    const Layout& layout = walker.program().at(classLayout);
    ClassFacts facts = walker.facts().classOf(classLayout);
    parent = { };
    switch (facts.baseState) {
    case BaseState::Layout:
        parent = facts.base;
        break;
    case BaseState::Unknown:
        parent = writtenBaseOf(layout);
        break;
    case BaseState::None:
        parent = facts.prototypeBase;
        break;
    case BaseState::BuiltinClass:
        return false;
    }
    return parent || !layout.has(LayoutFlags::HasHeritage);
}

// That a chain meets the class is proven by Link, link by link, or said by the guard of the class that the walk starts from, which answers for every link above it. That a chain misses the class only Link proves.
// `function Meter() {}`, `Meter.prototype = first`, `class Fine extends Meter {}`, `Meter.prototype = second`: the class below is linked to the object that the property held when it was defined. So no chain is followed up into a constructor function, or out of one.
static ChainEndForInstanceOf followChainForInstanceOf(const Walker& walker, LayoutID layout, LayoutID tested, ConditionsOfInstanceOf& conditions)
{
    const Program& program = walker.program();
    const LinkTables& link = walker.facts().linked();
    const Layout& start = program.at(layout);
    if (start.has(LayoutFlags::IsOfCreatedObject))
        return ChainEndForInstanceOf::Unknown;
    // Above the object of a literal stands Object.prototype, which is the prototype object of no class of the program.
    if (start.kind == LayoutKind::Record)
        return !start.constructor && !start.has(LayoutFlags::IsConstructorFunction) ? ChainEndForInstanceOf::MissesClass : ChainEndForInstanceOf::Unknown;
    if (start.kind != LayoutKind::Instance)
        return ChainEndForInstanceOf::Unknown;

    const LayoutID classWalkedFrom = declaredLayoutOf(program, layout);
    LayoutID current = classWalkedFrom;
    bool everyLinkIsProven = true;
    for (size_t unvisitedClasses = program.layouts.size(); current && unvisitedClasses; --unvisitedClasses) {
        if (current == tested)
            return ChainEndForInstanceOf::MeetsClass;
        const Layout& passed = program.at(current);
        if (passed.kind != LayoutKind::Instance || !passed.constructor)
            return ChainEndForInstanceOf::Unknown;
        // A row of functions: each prototype object is settled, and linked below the next, ahead of every object. Above the last stands Object.prototype: whatever else is laid above a prototype object lets the layout go.
        if (passed.has(LayoutFlags::IsConstructorFunction)) {
            const LinkedLayout& linked = link.layouts[current.index()];
            if (!everyLinkIsProven || !linked.prototypeObjectIsSettledAheadOfEveryObject)
                return ChainEndForInstanceOf::Unknown;
            if (!passed.base)
                return ChainEndForInstanceOf::MissesClass;
            if (!linked.isLinkedBelowBaseAheadOfEveryObject)
                return ChainEndForInstanceOf::Unknown;
            current = passed.base;
            continue;
        }

        LayoutID next = classProvenAbove(program, link, current);
        if (!next) {
            if (classStaysAsWrittenForInstanceOf(link, current))
                return everyLinkIsProven && !passed.has(LayoutFlags::HasHeritage) ? ChainEndForInstanceOf::MissesClass : ChainEndForInstanceOf::Unknown;
            if (!walker.plan().hasClassRecord(classWalkedFrom) || !walker.plan().hasClassRecord(current) || !findParentNamedByClassRecordForInstanceOf(walker, current, next) || !next)
                return ChainEndForInstanceOf::Unknown;
            everyLinkIsProven = false;
            addConditionOfInstanceOf(conditions, { MemberConditionKind::PrototypeLinkUnchanged, classWalkedFrom, ClassSide::Prototype, { } });
        }
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
    // `F.prototype = Object.prototype` ahead of the first `new F`: the guard of the property begins with whatever lies there, which other objects may stand below. No code can change the property of a class.
    bool prototypeObjectMayBeShared = program.at(tested).has(LayoutFlags::IsConstructorFunction) && !link.layouts[tested.index()].prototypeObjectIsSettledAheadOfEveryObject;
    ChainsForInstanceOf chains;
    for (LayoutID layout : ascendingLayouts) {
        // Whether a layout has objects is the plan's to say: it gives objects to a member of a tagged family, which no body names.
        if (!walker.plan().isInUse(layout))
            continue;

        ConditionsOfInstanceOf conditions;
        ChainEndForInstanceOf end = followChainForInstanceOf(walker, layout, tested, conditions);
        // The run time defines the class of a constructor function at the first construction that is carried out, with whatever the function has as its `prototype` then. What is made once that is replaced says so itself.
        bool isMadeByConstructorFunction = program.at(layout).kind == LayoutKind::Instance && program.at(declaredLayoutOf(program, layout)).has(LayoutFlags::IsConstructorFunction)
            && !link.layouts[declaredLayoutOf(program, layout).index()].prototypeObjectIsSettledAheadOfEveryObject;
        bool mayGetOtherPrototype = possibleChangesForInstanceOf(link, layout).contains(LayoutChange::ChangesPrototype);
        bool testsObject = false;
        if (end == ChainEndForInstanceOf::MissesClass && (mayGetOtherPrototype || prototypeObjectMayBeShared))
            end = ChainEndForInstanceOf::Unknown;
        if (end == ChainEndForInstanceOf::MeetsClass && (isMadeByConstructorFunction || mayGetOtherPrototype)) {
            // An object below a built-in class is a cell of that class: it has no shape word for a test of the object to rely on.
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
        for (const MemberConditionRecord& condition : conditions)
            addConditionOfInstanceOf(chains.conditions, condition);
        chains.testsObjects |= testsObject;
    }
    return chains;
}

// ---- A class of the program: what the operation becomes

// A proxy answers for its prototype with that of its target, or with code of the program, and so does whatever stands below one. A built-in that is not listed may be trimmed out of the executable, so the list is whole.
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

// A test of the shape asks for shape numbers, so it refuses every object of a layout that the plan gave none.
static bool isTestableByShapeForInstanceOf(const Walker& walker, std::span<const LayoutID> layouts)
{
    return std::ranges::all_of(layouts, [&](LayoutID layout) {
        return isFoundByShapeTest(walker.program().at(layout)) && walker.plan().slots().holdsCompiledObjects(layout) && !walker.plan().shapes().shapesOf(layout).isEmpty();
    });
}

// By the types alone, so that what is asked ahead and what is built agree. Nothing: the operation stays as written.
static std::optional<FormOfInstanceOf> planFormOfInstanceOf(const Walker& walker, TypeID typeOfValue, TypeID typeOfConstructor)
{
    // The class object of another instance of the program is a closure of the same function, and has a prototype object and guards of its own.
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

    // Ascending.
    Vector<LayoutID, 8> considered;
    // No object with another layout arrives.
    bool areAllConsidered = !value.hasObjects();
    if (value.hasObjects()) {
        Type objects = types.at(types.withOnlyObjects(typeOfValue));
        areAllConsidered = objects.hasOnlyLayouts() && !objects.traitsOfNamed.contains(ObjectTrait::MayHaveLayoutBelow);
        if (areAllConsidered)
            considered.append(membersOf(types, objects.layouts));
        else {
            // Nothing closes the layouts of the object. Those of the class and of what is written below it are worth one test.
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
    for (const MemberConditionRecord& condition : chains.conditions)
        addConditionOfInstanceOf(form.conditions, condition);
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
    // What holds only while guards of classes hold waits for whoever builds their test: withdrawn/ has it.
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
        // Both lookups on the class object are answered, so what is left of the operation is the walk up the chain of the value.
        form->instances = { };
        form->missMeansFalse = false;
        form->areAllInstances = false;
    }
    // Off a constructor function the walk of the chain is the operation as written: nothing would be gained.
    if (form->testedIsConstructorFunction && !form->instances && !form->missMeansFalse)
        return false;
    if (inputs.areOnlyTypes())
        return true;

    Reference value = walker.referenceOf(test.valueOperand());
    Reference constructor = walker.referenceOf(test.constructorOperand());
    if (!value || !value.isRecord() || !constructor || !constructor.isRecord())
        return false;
    // Only a value has a name that a test narrows.
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

    // `this instanceof F` behind its own allocation: the state knows that the object stands where it was made.
    bool testsPrototype = form->testsPrototypeOfInstance && !walker.state().factsOfCell(value).contains(FactOfCell::HasLayoutPrototype);
    Reference answer;
    if (!form->instances)
        answer = buildAnswerOfMiss();
    else if (form->areAllInstances && form->valueIsSurelyObject && !testsPrototype && walker.has(Mechanism::Folding))
        answer = booleanAnswerOfInstanceOf(walker, true);
    else {
        // A branch on the test itself narrows the value: to the layouts of the instances where it is taken, and on the other edge by what would surely have passed.
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

// ---- A built-in class: the right side

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
    // The ordinary function hands over to a hook of the host's File, which asks the blob behind the value and walks no chain.
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

// ---- A built-in class: the chain above an object

// Nothing stands above Object.prototype, and the language lets nobody lay it below another object.
static AnswerOfBuiltinInstanceOf answerRightBelowObjectPrototypeForInstanceOf(const BuiltinTable& table, BuiltinID soughtPrototype)
{
    return table.canonicalID(Builtins::ObjectPrototype) == soughtPrototype ? AnswerOfBuiltinInstanceOf::Yes : AnswerOfBuiltinInstanceOf::No;
}

// Of an object right below the prototype object of its layout. No prototype object of a class of the program is the one sought, so each is passed where Link has proven its own link. What stands at `extends` beside a class of the program is only written.
static AnswerOfBuiltinInstanceOf searchAboveLayoutForBuiltinInstanceOf(const Walker& walker, LayoutID layout, BuiltinID soughtPrototype)
{
    const Program& program = walker.program();
    const BuiltinTable& table = program.builtins();
    const LinkTables& link = walker.facts().linked();
    const Layout& start = program.at(layout);
    bool isMadeBelowWhatCodeChooses = start.has(LayoutFlags::IsOfCreatedObject) || (start.kind == LayoutKind::Record && (start.constructor || start.has(LayoutFlags::IsConstructorFunction)));
    if (isMadeBelowWhatCodeChooses || (start.kind != LayoutKind::Record && start.kind != LayoutKind::Instance))
        return AnswerOfBuiltinInstanceOf::Unknown;
    if (start.kind == LayoutKind::Record)
        return answerRightBelowObjectPrototypeForInstanceOf(table, soughtPrototype);

    LayoutID current = declaredLayoutOf(program, layout);
    for (size_t unvisitedClasses = program.layouts.size(); current && unvisitedClasses; --unvisitedClasses) {
        const Layout& passed = program.at(current);
        // `function Meter() {}`, `Meter.prototype = other`: which object stands above an instance goes by when that was made.
        if (passed.kind != LayoutKind::Instance || !passed.constructor || passed.has(LayoutFlags::IsConstructorFunction) || !classStaysAsWrittenForInstanceOf(link, current))
            return AnswerOfBuiltinInstanceOf::Unknown;
        if (LayoutID above = passed.base) {
            current = above;
            continue;
        }
        if (passed.has(LayoutFlags::HasHeritage) || passed.writtenBuiltinBase)
            return AnswerOfBuiltinInstanceOf::Unknown;
        return answerRightBelowObjectPrototypeForInstanceOf(table, soughtPrototype);
    }
    return AnswerOfBuiltinInstanceOf::Unknown;
}

// Any object may be a proxy. A cell that is known by its kind alone may have been given any prototype, at its creation or since, and no record says what stands above a named object.
static AnswerOfBuiltinInstanceOf answerOfBuiltinInstanceOfForType(const Walker& walker, const BuiltinTestedByInstanceOf& tested, TypeID typeOfValue)
{
    Type subject = walker.types().at(typeOfValue);
    if (!subject.isJavaScriptValue() || subject.mayBeUnspecified())
        return AnswerOfBuiltinInstanceOf::Unknown;
    // The ordinary @@hasInstance turns a primitive away before it reads anything.
    if (!subject.hasObjects())
        return AnswerOfBuiltinInstanceOf::No;
    if (!tested.prototype || subject.hasUnboundedUnknown() || subject.unnamedClasses || subject.objects || !subject.unnamedLayouts)
        return AnswerOfBuiltinInstanceOf::Unknown;

    const LinkTables& link = walker.facts().linked();
    std::optional<AnswerOfBuiltinInstanceOf> joined;
    for (LayoutID layout : membersOf(walker.types(), subject.unnamedLayouts)) {
        if (!walker.plan().isInUse(layout))
            continue;
        if (possibleChangesForInstanceOf(link, layout).contains(LayoutChange::ChangesPrototype))
            return AnswerOfBuiltinInstanceOf::Unknown;
        AnswerOfBuiltinInstanceOf ofLayout = searchAboveLayoutForBuiltinInstanceOf(walker, layout, tested.prototype);
        if (ofLayout == AnswerOfBuiltinInstanceOf::Unknown || (joined && *joined != ofLayout))
            return AnswerOfBuiltinInstanceOf::Unknown;
        joined = ofLayout;
    }
    if (!joined)
        return AnswerOfBuiltinInstanceOf::Unknown;
    if (*joined != AnswerOfBuiltinInstanceOf::Yes)
        return *joined;
    // An object of another instance of the program inherits from the prototype objects of that one's realm.
    AnswerOfBuiltinInstanceOf inOneRealm = subject.primitives ? AnswerOfBuiltinInstanceOf::YesIfObject : AnswerOfBuiltinInstanceOf::Yes;
    return walker.plan().hasOneInstance() ? inOneRealm : AnswerOfBuiltinInstanceOf::Unknown;
}

// A cell of the class that is as its class makes it has the prototype that its class gave it. The state keeps what the test has found only while EscapeAnalysis is on, and the test is built by the type of its operand.
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
    // The code of a TypeTest asks for the testable part of the type, which is wider where nothing tells the cells of the class from others.
    return (hasInstanceTest(table, tested.builtinClass) && types.testablePartOf(instances) == instances) || types.isSubset(typeOfValue, instances);
}

static Reference buildWalkOfChainToBuiltinForInstanceOf(Walker& walker, Reference value, BuiltinID prototype, Origin origin)
{
    Draft found(Opcode::Builtin, origin);
    walker.chooseConservativeFields(found);
    found.setBuiltin(prototype);
    return buildWalkOfChainForInstanceOf(walker, value, walker.build(WTF::move(found)), origin);
}

// A test that fails says nothing of the chain: an object whose prototype was set may be an instance.
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

// The lookup of @@hasInstance is answered, and no constant stands for the object: the steps of the ordinary @@hasInstance are written out in its order.
// Where it finds no object under `prototype` it throws, which is left to the operation as written.
static Reference buildWalkOfChainBehindReadOfPrototypeForInstanceOf(Walker& walker, const BodyValue& test, Reference value, Reference constructor, Origin origin)
{
    AnswersOfInstanceOf answers(walker);
    walker.noteGuessOpened();
    if (walker.types().at(walker.typeOf(value)).primitives) {
        Reference isObject = buildTypeTestOfInstanceOf(walker, value, TypeTable::object, origin);
        if (!isObject)
            return { };
        BlockID object = walker.addBlock();
        BlockID primitive = walker.addBlock();
        walker.branch(isObject, object, primitive);
        walker.continueIn(primitive);
        answers.arrive(booleanAnswerOfInstanceOf(walker, false));
        walker.continueIn(object);
    }

    Draft read(Opcode::PropGet, origin);
    walker.chooseConservativeFields(read);
    read.setObjectOperand(constructor);
    read.setKey(walker.program().keyOf(BuiltinNames::prototype));
    Reference prototype = walker.build(WTF::move(read));
    Reference isObject = prototype && prototype.isValue() ? buildTypeTestOfInstanceOf(walker, prototype, TypeTable::object, origin) : Reference();
    if (!isObject)
        return { };
    BlockID findsObject = walker.addBlock();
    BlockID findsNoObject = walker.addBlock(BlockFrequency::Rare);
    walker.branch(isObject, findsObject, findsNoObject);
    walker.continueIn(findsNoObject);
    answers.arrive(buildInstanceOfAsWritten(walker, test));
    walker.continueIn(findsObject);
    answers.arrive(buildWalkOfChainForInstanceOf(walker, value, buildProofOfInstanceOf(walker, prototype, isObject, OperandIndex::TypeTest::value, origin), origin));
    return answers.meet();
}

// buildInstanceOfBuiltin() keeps the rest: the walk of the chain from whatever the value is.
static bool buildInstanceOfBuiltinByValue(Walker& walker, const BodyValue& test, const InputsOfTypedOperation& inputs)
{
    TypeID typeOfValue = inputs.typeOf(test.valueOperand());
    TypeID typeOfConstructor = inputs.typeOf(test.constructorOperand());
    if (!typeOfValue || !typeOfConstructor)
        return false;
    std::optional<BuiltinTestedByInstanceOf> tested = describeBuiltinTestedByInstanceOf(walker, typeOfConstructor);
    if (!tested)
        return false;

    AnswerOfBuiltinInstanceOf byType = answerOfBuiltinInstanceOfForType(walker, *tested, typeOfValue);
    if (byType != AnswerOfBuiltinInstanceOf::YesIfObject && !walker.has(Mechanism::Folding))
        byType = AnswerOfBuiltinInstanceOf::Unknown;
    bool testsClass = false;
    bool readsPrototype = false;
    if (byType == AnswerOfBuiltinInstanceOf::Unknown && mayBuildArmsForInstanceOf(walker)) {
        if (!tested->prototype)
            readsPrototype = walker.isInLoop();
        else if (classTestAnswersBuiltinInstanceOf(walker, *tested, typeOfValue)) {
            // A bit test and a rare call in the place of a call: where the code comes again and again, or the value is expected to be of the class.
            TypeID expected = walker.isInLoop() ? TypeID() : findExpectation(walker, test.valueOperand()).type;
            testsClass = walker.isInLoop() || (expected && walker.types().overlap(expected, walker.types().builtinClass(tested->builtinClass)));
        }
    }
    if (inputs.areOnlyTypes())
        return byType != AnswerOfBuiltinInstanceOf::Unknown || testsClass || readsPrototype;

    Reference value = walker.referenceOf(test.valueOperand());
    Reference constructor = walker.referenceOf(test.constructorOperand());
    if (!value || !value.isRecord() || !constructor || !constructor.isRecord())
        return false;
    Origin origin = walker.originOf(test);
    Reference answer;
    switch (byType) {
    case AnswerOfBuiltinInstanceOf::No:
        answer = booleanAnswerOfInstanceOf(walker, false);
        break;
    case AnswerOfBuiltinInstanceOf::Yes:
        answer = booleanAnswerOfInstanceOf(walker, true);
        break;
    case AnswerOfBuiltinInstanceOf::YesIfObject:
        answer = buildTypeTestOfInstanceOf(walker, value, TypeTable::object, origin);
        break;
    case AnswerOfBuiltinInstanceOf::Unknown: {
        // The state has seen the object as its class makes it.
        bool isSeenAsMade = tested->builtinClass && walker.has(Mechanism::Folding) && walker.plan().hasOneInstance() && walker.types().isSubset(walker.typeOf(value), walker.types().builtinClass(tested->builtinClass)) && isListAsMade(walker, value);
        if (isSeenAsMade)
            answer = booleanAnswerOfInstanceOf(walker, true);
        else if (testsClass && value.isValue())
            answer = buildClassTestsOfBuiltinInstanceOf(walker, *tested, value, origin);
        else if (readsPrototype && value.isValue())
            answer = buildWalkOfChainBehindReadOfPrototypeForInstanceOf(walker, test, value, constructor, origin);
        else
            return false;
        break;
    }
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
