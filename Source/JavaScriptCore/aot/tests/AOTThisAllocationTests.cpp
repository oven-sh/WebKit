/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTClassTable.h"
#include "AOTClosure.h"
#include "AOTClosureTable.h"
#include "AOTCompileOptionWords.h"
#include "AOTDriver.h"
#include "AOTIRParser.h"
#include "AOTImage.h"
#include "AOTKeyOrderTable.h"
#include "AOTLoadedProgramTestSupport.h"
#include "AOTNativeTestReasons.h"
#include "AOTObject.h"
#include "AOTObjectModel.h"
#include "AOTOptionWords.h"
#include "AOTProgramFile.h"
#include "AOTShellProcess.h"
#include "AOTStubTestSupport.h"
#include "AOTTemporaryFile.h"
#include "AOTTestHarness.h"
#include "AOTThisAllocationInlines.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include "DeferGCInlines.h"
#include "FunctionPrototype.h"
#include "JSCInlines.h"
#include "ObjectConstructor.h"
#include "StructureCache.h"
#include <array>
#include <initializer_list>
#include <stdlib.h>
#include <wtf/DataLog.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

std::span<const NativeTest> thisAllocationTests();

static constexpr ASCIILiteral enumeratedClassesText = R"IR(layout L0 kind=Instance name="Base" module=m0 constructor=f1 fields=[{key="x" declared={type=anything}}, {key="label" declared={type=anything}}, {key="word" declared={type=anything}}, {key="list" declared={type=anything}}]
layout L1 kind=Instance name="Derived" module=m0 base=L0 constructor=f2 fields=[{key="y" declared={type=anything}}] flags=[HasHeritage]
layout L2 kind=Instance name="Unrelated" module=m0 constructor=f3 fields=[{key="u" declared={type=anything}}]
layout L3 kind=Instance name="Point" module=m0 constructor=f4 fields=[{key="px" declared={type=anything}}, {key="py" declared={type=anything}}] flags=[IsConstructorFunction]
layout L4 kind=Instance name="Below" module=m0 base=L3 constructor=f5 fields=[{key="z" declared={type=anything}}] flags=[HasHeritage]
variable g0 name="Point" module=m0 declared={type=function} initialization=Function function=f4
module m0 path="/main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="publish" [isFreeName]
    v2 = NewClosure function=f1
    v3 = NewClass v2 layout=L0
    v4 = FinishClass v3 layout=L0
    v5 = NewClosure function=f2
    v6 = ResolveHeritage v4
    v7 = NewClass v5, v4, v6 layout=L1
    v8 = FinishClass v7 layout=L1
    v9 = NewClosure function=f3
    v10 = NewClass v9 layout=L2
    v11 = FinishClass v10 layout=L2
    v12 = GetModuleVariable variable=g0
    v13 = NewClosure function=f5
    v14 = ResolveHeritage v12
    v15 = NewClass v13, v12, v14 layout=L4
    v16 = FinishClass v15 layout=L4
    v17 = Call v1, -, v4
    v18 = Call v1, -, v11
    v19 = Call v1, -, v12
    v20 = Construct v4
    v21 = Call v1, -, v20
    v22 = Construct v8
    v23 = Call v1, -, v22
    v24 = Construct v11
    v25 = Call v1, -, v24
    v26 = Construct v12
    v27 = Call v1, -, v26
    v28 = Call v1, -, v16
    v29 = Construct v16
    v30 = Call v1, -, v29
    Return
}
function f1 name="Base" kind=BaseConstructor flags=[HasNewTarget] results=[{type=anything}] {
    v1 = GetConstructPrototype v0 layout=L0
    v2 = NewObject v1 layout=L0
    v3 = Integer integer=1
    PropSet v2, v3 key="x" mode=Strict
    v5 = Builtin builtin=GlobalThis
    v6 = PropGet v5 key="input" [isFreeName]
    PropSet v2, v6 key="label" mode=Strict
    v8 = String string="n"
    PropSet v2, v8 key="word" mode=Strict
    v10 = NewArray v3 contents=anything
    PropSet v2, v10 key="list" mode=Strict
    Return v2
}
function f2 name="Derived" kind=DerivedConstructor flags=[HasNewTarget, TakesCallee] results=[{type=anything}] {
    v2 = GetSuperBase v0 homeKind=Object
    v3 = SuperConstruct v2, v1 layout=L1
    v4 = Integer integer=2
    PropSet v3, v4 key="y" mode=Strict
    Return v3
}
function f3 name="Unrelated" kind=BaseConstructor flags=[HasNewTarget] results=[{type=anything}] {
    v1 = GetConstructPrototype v0 layout=L2
    v2 = NewObject v1 layout=L2
    v3 = Integer integer=3
    PropSet v2, v3 key="u" mode=Strict
    Return v2
}
function f4 name="Point" flags=[HasThis] thisType={type=L3 flags=[IsExpected]} results=[{type=anything}] {
    v1 = Integer integer=4
    PropSet v0, v1 key="py" mode=Strict
    v3 = Integer integer=5
    PropSet v0, v3 key="px" mode=Strict
    Return
}
function f5 name="Below" kind=DerivedConstructor flags=[HasNewTarget, TakesCallee] results=[{type=anything}] {
    v2 = GetSuperBase v0 homeKind=Object
    v3 = SuperConstruct v2, v1 layout=L4
    v4 = Integer integer=6
    PropSet v3, v4 key="z" mode=Strict
    Return v3
}
)IR"_s;

static constexpr ASCIILiteral neverEnumeratedClassesText = R"IR(layout L0 kind=Instance name="Base" module=m0 constructor=f1 fields=[{key="x" declared={type=anything}}, {key="on" declared={type=anything}}]
layout L1 kind=Instance name="Derived" module=m0 base=L0 constructor=f2 fields=[{key="y" declared={type=anything}}] flags=[HasHeritage]
layout L2 kind=Instance name="Other" module=m0 base=L0 constructor=f3 fields=[{key="z" declared={type=anything}}] flags=[HasHeritage]
module m0 path="/main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="publish" [isFreeName]
    v2 = NewClosure function=f1
    v3 = NewClass v2 layout=L0
    v4 = FinishClass v3 layout=L0
    v5 = NewClosure function=f2
    v6 = ResolveHeritage v4
    v7 = NewClass v5, v4, v6 layout=L1
    v8 = FinishClass v7 layout=L1
    v9 = NewClosure function=f3
    v10 = ResolveHeritage v4
    v11 = NewClass v9, v4, v10 layout=L2
    v12 = FinishClass v11 layout=L2
    v13 = Builtin builtin=MathRandom
    v14 = Call v13
    v15 = Integer integer=2
    v16 = Less v14, v15
    Branch v16 -> taken b1(), notTaken b2()
  b1():
    Jump -> next b3(v8)
  b2():
    Jump -> next b3(v12)
  b3(v17: anything):
    v18 = Construct v17
    v19 = PropGet v18 key="x"
    v20 = PropGet v18 key="on"
    v21 = PropGet v18 key="y"
    v22 = PropGet v18 key="z"
    v23 = Call v1, -, v19, v20, v21, v22
    Return
}
function f1 name="Base" kind=BaseConstructor flags=[HasNewTarget] results=[{type=anything}] {
    v1 = GetConstructPrototype v0 layout=L0
    v2 = NewObject v1 layout=L0
    v3 = Integer integer=1
    PropSet v2, v3 key="x" mode=Strict
    v5 = Boolean boolean=true
    PropSet v2, v5 key="on" mode=Strict
    Return v2
}
function f2 name="Derived" kind=DerivedConstructor flags=[HasNewTarget, TakesCallee] results=[{type=anything}] {
    v2 = GetSuperBase v0 homeKind=Object
    v3 = SuperConstruct v2, v1 layout=L1
    v4 = Integer integer=2
    PropSet v3, v4 key="y" mode=Strict
    Return v3
}
function f3 name="Other" kind=DerivedConstructor flags=[HasNewTarget, TakesCallee] results=[{type=anything}] {
    v2 = GetSuperBase v0 homeKind=Object
    v3 = SuperConstruct v2, v1 layout=L2
    v4 = Integer integer=3
    PropSet v3, v4 key="z" mode=Strict
    Return v3
}
)IR"_s;

static constexpr ASCIILiteral closureOverInstanceText = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
layout L0 kind=Instance name="Counter" constructor=f1 fields=[{key="run" declared={type=function} flags=[IsRequired]}, {key="x" declared={type=anything}}]
variable g0 name="kept" module=m0 declared={type=anything} initialization=Undefined
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="print" [isFreeName]
    v2 = NewClosure function=f1
    v3 = NewClass v2 layout=L0
    v4 = FinishClass v3 layout=L0
    v5 = Construct v4
    PutModuleVariable v5 variable=g0
    v7 = GetModuleVariable variable=g0
    v8 = PropGet v7 key="run"
    v9 = Call v8, v7 calleeKey="run"
    v10 = PropGet v7 key="run"
    v11 = StrictEq v8, v10
    v12 = Call v1, -, v9, v11
    Return
}
function f1 name="Counter" kind=BaseConstructor flags=[HasNewTarget] results=[{type=anything}] {
    v1 = GetConstructPrototype v0 layout=L0
    v2 = NewObject v1 layout=L0
    v3 = NewClosure v2 function=f2
    PropDefine v2, v3 key="run"
    v5 = Integer integer=7
    PropSet v2, v5 key="x" mode=Strict
    Return v2
}
function f2 name="run" kind=Arrow captures=[{type=L0}] results=[{type=anything}] {
    v1 = PropGet v0 key="x"
    Return v1
}
)IR"_s;

static constexpr ASCIILiteral scriptForEnumeratedClasses = R"js(
let constructors = [];
globalThis.input = "text";
function nameOfPrototype(object)
{
    for (let i = 0; i < constructors.length; ++i) {
        if (constructors[i].prototype === Object.getPrototypeOf(object))
            return constructors[i].name;
    }
    return "of no published constructor";
}
function show(label, object)
{
    let namesOfLoop = [];
    for (let name in object)
        namesOfLoop[namesOfLoop.length] = name;
    print(label + ": keys " + Object.keys(object).join() + " for-in " + namesOfLoop.join() + " JSON " + JSON.stringify(object) + " prototype " + nameOfPrototype(object));
}
globalThis.publish = value => {
    if (typeof value == "function") {
        constructors[constructors.length] = value;
        return;
    }
    if (value.constructor.name == "Derived")
        constructors[constructors.length] = value.constructor;
    show("compiled new " + value.constructor.name, value);
};
function constructFromNativeCode()
{
    for (let i = 0; i < constructors.length; ++i) {
        show("new " + constructors[i].name, new constructors[i]);
        for (let j = 0; j < constructors.length; ++j) {
            if (i != j)
                show("Reflect.construct(" + constructors[i].name + ", [], " + constructors[j].name + ")", Reflect.construct(constructors[i], [], constructors[j]));
        }
    }
}
)js"_s;

static constexpr ASCIILiteral statementsAfterEnumeratedClasses = "constructFromNativeCode();"_s;

static constexpr ASCIILiteral outputOfEnumeratedClasses = R"(compiled new Base: keys x,label,word,list for-in x,label,word,list JSON {"x":1,"label":"text","word":"n","list":[1]} prototype Base
compiled new Derived: keys x,label,word,list,y for-in x,label,word,list,y JSON {"x":1,"label":"text","word":"n","list":[1],"y":2} prototype Derived
compiled new Unrelated: keys u for-in u JSON {"u":3} prototype Unrelated
compiled new Point: keys py,px for-in py,px JSON {"py":4,"px":5} prototype Point
compiled new Below: keys py,px,z for-in py,px,z JSON {"py":4,"px":5,"z":6} prototype Below
new Base: keys x,label,word,list for-in x,label,word,list JSON {"x":1,"label":"text","word":"n","list":[1]} prototype Base
Reflect.construct(Base, [], Unrelated): keys x,label,word,list for-in x,label,word,list JSON {"x":1,"label":"text","word":"n","list":[1]} prototype Unrelated
Reflect.construct(Base, [], Point): keys x,label,word,list for-in x,label,word,list JSON {"x":1,"label":"text","word":"n","list":[1]} prototype Point
Reflect.construct(Base, [], Derived): keys x,label,word,list for-in x,label,word,list JSON {"x":1,"label":"text","word":"n","list":[1]} prototype Derived
Reflect.construct(Base, [], Below): keys x,label,word,list for-in x,label,word,list JSON {"x":1,"label":"text","word":"n","list":[1]} prototype Below
new Unrelated: keys u for-in u JSON {"u":3} prototype Unrelated
Reflect.construct(Unrelated, [], Base): keys u for-in u JSON {"u":3} prototype Base
Reflect.construct(Unrelated, [], Point): keys u for-in u JSON {"u":3} prototype Point
Reflect.construct(Unrelated, [], Derived): keys u for-in u JSON {"u":3} prototype Derived
Reflect.construct(Unrelated, [], Below): keys u for-in u JSON {"u":3} prototype Below
new Point: keys py,px for-in py,px JSON {"py":4,"px":5} prototype Point
Reflect.construct(Point, [], Base): keys py,px for-in py,px JSON {"py":4,"px":5} prototype Base
Reflect.construct(Point, [], Unrelated): keys py,px for-in py,px JSON {"py":4,"px":5} prototype Unrelated
Reflect.construct(Point, [], Derived): keys py,px for-in py,px JSON {"py":4,"px":5} prototype Derived
Reflect.construct(Point, [], Below): keys py,px for-in py,px JSON {"py":4,"px":5} prototype Below
new Derived: keys x,label,word,list,y for-in x,label,word,list,y JSON {"x":1,"label":"text","word":"n","list":[1],"y":2} prototype Derived
Reflect.construct(Derived, [], Base): keys x,label,word,list,y for-in x,label,word,list,y JSON {"x":1,"label":"text","word":"n","list":[1],"y":2} prototype Base
Reflect.construct(Derived, [], Unrelated): keys x,label,word,list,y for-in x,label,word,list,y JSON {"x":1,"label":"text","word":"n","list":[1],"y":2} prototype Unrelated
Reflect.construct(Derived, [], Point): keys x,label,word,list,y for-in x,label,word,list,y JSON {"x":1,"label":"text","word":"n","list":[1],"y":2} prototype Point
Reflect.construct(Derived, [], Below): keys x,label,word,list,y for-in x,label,word,list,y JSON {"x":1,"label":"text","word":"n","list":[1],"y":2} prototype Below
new Below: keys py,px,z for-in py,px,z JSON {"py":4,"px":5,"z":6} prototype Below
Reflect.construct(Below, [], Base): keys py,px,z for-in py,px,z JSON {"py":4,"px":5,"z":6} prototype Base
Reflect.construct(Below, [], Unrelated): keys py,px,z for-in py,px,z JSON {"py":4,"px":5,"z":6} prototype Unrelated
Reflect.construct(Below, [], Point): keys py,px,z for-in py,px,z JSON {"py":4,"px":5,"z":6} prototype Point
Reflect.construct(Below, [], Derived): keys py,px,z for-in py,px,z JSON {"py":4,"px":5,"z":6} prototype Derived
)"_s;

static constexpr ASCIILiteral scriptForNeverEnumeratedClasses = "globalThis.publish = (x, on, y, z) => print([x, on, y, z].join());"_s;

static constexpr ASCIILiteral outputOfNeverEnumeratedClasses = "1,true,2,\n"_s;

namespace {

struct ShellRun {
    ASCIILiteral programText;
    ASCIILiteral script;
    ASCIILiteral statementsAfterProgram;
    ASCIILiteral expectedOutput;
    bool scriptUsesWhatTheProgramCreates { false };
};

struct ListedField {
    ASCIILiteral name;
    JSValue value;
};

} // anonymous namespace

static ClassTable& classesOf(LoadedProgram& program)
{
    return program.state().objects->classes;
}

static const LayoutData& classNamed(LoadedProgram& program, ASCIILiteral name)
{
    const ProgramTables& tables = program.state().tables;
    for (uint32_t shape = 1; shape < tables.firstEnvironmentShape; ++shape) {
        if (tables.hasLayoutForShape(shape) && isNamedInTextOfLoadedProgram(tables.layoutForShape(shape), name))
            return tables.layoutForShape(shape);
    }
    failNativeTest(__FILE__, __LINE__, name.characters());
}

static const FieldData& fieldNamed(LoadedProgram& program, const LayoutData& layout, ASCIILiteral name)
{
    VM& vm = program.vm();
    const FieldData* field = program.state().objects->layouts.field(vm, layout, Identifier::fromString(vm, name).impl());
    AOT_TEST_CHECK(field);
    return *field;
}

static JSObject* constructorOf(LoadedProgram& program, ASCIILiteral className)
{
    FunctionID constructor(classNamed(program, className).classData().classConstructor);
    return asObject(program.state().objects->closures.closure(program.vm(), constructor));
}

static JSObject* defineClass(LoadedProgram& program, ASCIILiteral className, JSObject* parent)
{
    VM& vm = program.vm();
    ObjectModel& objects = *program.state().objects;
    FunctionID constructor(classNamed(program, className).classData().classConstructor);
    Closure* classObject = Closure::create(vm, objects.closures.closureStructureFor(vm, constructor), constructor);
    objects.classes.defineClass(program.realm(), classObject, parent ? JSValue(parent) : JSValue(), parent ? parent->get(program.realm(), vm.propertyNames->prototype) : JSValue());
    AOT_TEST_CHECK(!vm.exceptionForInspection());
    return classObject;
}

static JSObject* classObjectOf(LoadedProgram& program, ASCIILiteral className)
{
    FunctionID constructor(classNamed(program, className).classData().classConstructor);
    if (JSCell* registered = program.state().objects->closures.closureIfCreated(constructor))
        return asObject(registered);
    bool extendsBase = className == "Derived"_s || className == "Other"_s;
    return defineClass(program, className, extendsBase ? classObjectOf(program, "Base"_s) : nullptr);
}

static JSObject* prototypeOf(LoadedProgram& program, ASCIILiteral className)
{
    if (JSObject* prototype = classesOf(program).classPrototype(program.vm(), classNamed(program, className)))
        return prototype;
    JSValue prototype = classObjectOf(program, className)->get(program.realm(), program.vm().propertyNames->prototype);
    AOT_TEST_CHECK(prototype.isObject() && classesOf(program).classPrototypeIfExists(classNamed(program, className)) == asObject(prototype));
    return asObject(prototype);
}

static Object* allocate(LoadedProgram& program, ASCIILiteral className, std::initializer_list<ListedField> listed)
{
    VM& vm = program.vm();
    ObjectModel& objects = *program.state().objects;
    const LayoutData& layout = classNamed(program, className);
    Vector<uint16_t> entries;
    for (const ListedField& field : listed)
        entries.append(layout.indexOf(fieldNamed(program, layout, field.name)));
    std::optional<uint32_t> numbered = objects.keyOrders.orderStartingWith(layout, entries.span());
    AOT_TEST_CHECK(numbered);
    Object* object = Object::create(vm, objects.structures.objectStructure(vm, layout.shape), layout, layout.size, *numbered);
    for (const ListedField& field : listed)
        AOT_TEST_CHECK(object->store(vm, fieldNamed(program, layout, field.name), AcceptedValue::provenAtBuildTime(field.value)));
    return object;
}

static Object* instanceFor(LoadedProgram& program, Object* allocated, JSObject* prototype)
{
    JSObject* instance = classesOf(program).instanceWithPrototype(program.vm(), allocated, prototype);
    AOT_TEST_CHECK(isCompiledObject(instance));
    return uncheckedDowncast<Object>(instance);
}

static void checkWhichInstancesHaveThePrototypeOfTheirLayout(LoadedProgram& program)
{
    DeferGC deferGC(program.vm());
    Object* ofOwnClass = instanceFor(program, allocate(program, "Base"_s, { }), prototypeOf(program, "Base"_s));
    AOT_TEST_CHECK(&ofOwnClass->layout() == &classNamed(program, "Base"_s) && !ofOwnClass->compiledStructure().hasOtherPrototype());

    Object* ofSubclass = instanceFor(program, allocate(program, "Base"_s, { }), prototypeOf(program, "Derived"_s));
    AOT_TEST_CHECK(&ofSubclass->layout() == &classNamed(program, "Derived"_s) && !ofSubclass->compiledStructure().hasOtherPrototype());
    AOT_TEST_CHECK(ofSubclass->structure() == program.state().objects->structures.objectStructure(program.vm(), classNamed(program, "Derived"_s).shape));

    Object* withOtherPrototype = instanceFor(program, allocate(program, "Base"_s, { }), constructEmptyObject(program.realm()));
    AOT_TEST_CHECK(&withOtherPrototype->layout() == &classNamed(program, "Base"_s) && withOtherPrototype->compiledStructure().hasOtherPrototype());
}

#if OS(UNIX)
static constexpr ASCIILiteral classThatStaysInSightText = R"IR(layout L0 kind=Instance name="Kept" module=m0 constructor=f1 fields=[{key="x" declared={type=anything}}]
module m0 path="/main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = NewClosure function=f1
    v1 = NewClass v0 layout=L0
    v2 = FinishClass v1 layout=L0
    v3 = Construct v2
    v4 = PropGet v3 key="x"
    Return v4
}
function f1 name="Kept" kind=BaseConstructor flags=[HasNewTarget] results=[{type=anything}] {
    v1 = GetConstructPrototype v0 layout=L0
    v2 = NewObject v1 layout=L0
    v3 = Integer integer=1
    PropSet v2, v3 key="x" mode=Strict
    Return v2
}
)IR"_s;

static void checkStrangePrototypeAtBirthIsStoppedWherePrototypeStaysAsWritten(LoadedProgram& program)
{
    DeferGC deferGC(program.vm());
    AOT_TEST_CHECK(classNamed(program, "Kept"_s).impossibleMutations().contains(ObjectMutation::ChangePrototype));
    Object* allocated = allocate(program, "Kept"_s, { });
    JSObject* stranger = constructEmptyObject(program.realm());
    ChildOutcome outcome = runInChild([&] {
        classesOf(program).instanceWithPrototype(program.vm(), allocated, stranger);
    });
    AOT_TEST_CHECK(outcome == ChildOutcome::Crashed);
}
#endif

template<typename CreateSlow, typename TryCreateInline>
static bool createsSameCell(size_t size, const CreateSlow& createSlow, const TryCreateInline& tryCreateInline)
{
    constexpr unsigned maxNumberOfRefills = 100;
    for (unsigned refill = 0; refill < maxNumberOfRefills; ++refill) {
        JSObject* slow = createSlow();
        if (JSObject* fast = tryCreateInline())
            return fast != slow && !memcmp(fast, slow, size);
    }
    return false;
}

static void checkCreateThisFastPathsCreateSameCells(LoadedProgram& program)
{
    VM& vm = program.vm();
    DeferGC deferGC(vm);
    JSObject* constructor = constructorOf(program, "Point"_s);
    JSObject* object = classesOf(program).createThis(program.realm(), constructor, constructor);
    Structure* structure = rememberedStructureOfThis(vm, constructor, constructor);
    AOT_TEST_CHECK(structure && structure == object->structure());
    AOT_TEST_CHECK(compiledLayoutOrNull(structure) == &classNamed(program, "Point"_s));
    AOT_TEST_CHECK(!rememberedStructureOfThis(vm, constructor, constructorOf(program, "Unrelated"_s)));
    AOT_TEST_CHECK(!rememberedStructureOfThis(vm, program.realm()->objectConstructor(), program.realm()->objectConstructor()));

    for (unsigned inlineCapacity : { 0u, 1u, 5u, JSFinalObject::defaultInlineCapacity, 7u, 10u, 33u, JSFinalObject::maxInlineCapacity }) {
        Structure* ordinary = program.realm()->structureCache().emptyObjectStructureForPrototype(program.realm(), program.realm()->objectPrototype(), inlineCapacity);
        AOT_TEST_CHECK(createsSameCell(JSFinalObject::allocationSize(ordinary->inlineCapacity()), [&] {
            return constructEmptyObject(vm, ordinary);
        }, [&] {
            return tryCreateFinalObjectInline(vm, ordinary);
        }));
    }

    PutPropertySlot slot(constructor);
    constructor->methodTable()->put(constructor, program.realm(), vm.propertyNames->prototype, constructEmptyObject(program.realm()), slot);
    AOT_TEST_CHECK(!rememberedStructureOfThis(vm, constructor, constructor));
}

static void checkClassBelowConstructorFunction(LoadedProgram& program)
{
    DeferGC deferGC(program.vm());
    AOT_TEST_CHECK(classesOf(program).baseOf(classNamed(program, "Below"_s)) == &classNamed(program, "Point"_s));
    JSObject* function = constructorOf(program, "Point"_s);
    JSObject* classObject = defineClass(program, "Below"_s, function);
    AOT_TEST_CHECK(classObject->getPrototypeDirect() == JSValue(function));
    AOT_TEST_CHECK(prototypeOf(program, "Below"_s)->getPrototypeDirect() == function->get(program.realm(), program.vm().propertyNames->prototype));
    AOT_TEST_CHECK(constructorOf(program, "Unrelated"_s)->getPrototypeDirect() == JSValue(program.realm()->functionPrototype()));
}

static std::unique_ptr<CompileOutput> compileOnThisThread(const ShellRun& run, ASCIILiteral way, OptionWords optionWords)
{
    Wire::CompileOptions options = makeOptionsOfTestedProgram();
    if (run.scriptUsesWhatTheProgramCreates)
        AOT_TEST_CHECK(!applyOptionWord(options, "admit=Inspector"_s));
    for (ASCIILiteral word : optionWords)
        AOT_TEST_CHECK(!applyOptionWord(options, word));
    BuildContext build(options);
    std::unique_ptr<Typed::Program> program = Typed::parseProgram(Typed::withStandInLibrary(String(run.programText)), build);
    if (!program)
        failBecauseTextIsNoProgram(__FILE__, __LINE__, build, optionWordsOfReason(way, optionWords));

    std::unique_ptr<CompileOutput> output = Typed::compileWithMechanisms(WTF::move(program), build, Typed::mechanismsOfWay(way));
    if (!output->hasImage())
        failBecauseThereIsNoImage(__FILE__, __LINE__, *output, optionWordsOfReason(way, optionWords));
    AOT_TEST_CHECK(output->records<uint32_t>(Wire::ResultSection::EntryModuleIds).size() == 1);
    return output;
}

static void checkOutputInShell(const ShellRun& run, ASCIILiteral way, OptionWords optionWords)
{
    std::unique_ptr<CompileOutput> compiled;
    runOnBuildThread([&] {
        compiled = compileOnThisThread(run, way, optionWords);
    });

    TemporaryFile image(".image"_s, compiled->section(Wire::ResultSection::Image));
    Vector<String> arguments {
        makeString("--aotImagePath="_s, image.path()),
        String("-e"_s),
        makeString(run.script, "runCompiledEntryPoint("_s, compiled->records<uint32_t>(Wire::ResultSection::EntryModuleIds)[0], ").then(() => {"_s, run.statementsAfterProgram, "}).catch(error => print(\"the script threw \" + error));"_s),
    };
    String shell = shellPath();
    ChildProcessResult result = ChildProcessLauncher(String(shell), { }).run(arguments.span());
    String output = makeStringByReplacingAll(result.output, "\r\n"_s, "\n"_s);
    if (!result.exitedWith(EXIT_SUCCESS))
        dataLogLn(shell, " ", result.description(), "\n", output);
    checkThatChildSucceeded(__FILE__, __LINE__, optionWordsOfReason(way, optionWords), result, EXIT_SUCCESS);
    checkPrintedLines(__FILE__, __LINE__, optionWordsOfReason(way, optionWords), output, run.expectedOutput);
}

static void checkOutputInShellInEveryWay(const ShellRun& run)
{
    for (ASCIILiteral way : waysToCompilePrograms) {
        checkOutputInShell(run, way, { "references=wide"_s });
        checkOutputInShell(run, way, { "references=narrow"_s });
    }
}

static void testConstructionFromNativeCode()
{
    checkOutputInShellInEveryWay({ enumeratedClassesText, scriptForEnumeratedClasses, statementsAfterEnumeratedClasses, outputOfEnumeratedClasses, true });
}

static void testConstructionOfOneOfTwoSubclasses()
{
    checkOutputInShellInEveryWay({ neverEnumeratedClassesText, scriptForNeverEnumeratedClasses, ""_s, outputOfNeverEnumeratedClasses });
}

static void testDeferredClosureDefinedByName()
{
    checkOutputInShellInEveryWay({ closureOverInstanceText, ""_s, ""_s, "7 true\n"_s });
}

template<LoadedProgramTest body>
static constexpr void (*withEnumeratedClasses)() = runNativeTestWithLoadedProgram<thisAllocationTests, enumeratedClassesText, body>;

static constexpr ASCIILiteral noClassAboveInRecord = "facts/AOTFacts.h, classOf(): the class above in the record of a class: no class has BaseState::Layout"_s;

static void testPrototypesAtEvenStridesDoNotStandInARow()
{
    constexpr size_t numberOfPairs = 20000;
    constexpr size_t numberOfEntries = 65536;
    constexpr uint64_t mask = (numberOfEntries - 1) << shiftOfInstanceBelowPrototype;
    constexpr std::array<uint64_t, 3> blocksApart { 5, 5, 6 };
    Vector<uint64_t> taken(FillWith { }, numberOfEntries, static_cast<uint64_t>(0));
    size_t numberOfLooks = 0;
    uint64_t block = 0xd7845d00000;
    for (size_t pair = 0, turn = 0; pair < numberOfPairs; block += blocksApart[turn++ % blocksApart.size()] * MarkedBlock::blockSize) {
        for (uint64_t cell = 0; cell < 250 && pair < numberOfPairs; ++cell, ++pair) {
            uint64_t prototype = block + 384 + cell * 64;
            for (uint64_t offset = firstOffsetOfInstanceBelowPrototype(prototype, 3);; offset += sizeof(InstanceBelowPrototype)) {
                ++numberOfLooks;
                uint64_t& entry = taken[(offset & mask) >> shiftOfInstanceBelowPrototype];
                if (entry)
                    continue;
                entry = prototype;
                break;
            }
        }
    }
    AOT_TEST_CHECK(numberOfLooks < 4 * numberOfPairs);
}

std::span<const NativeTest> thisAllocationTests()
{
    static constexpr NativeTest tests[] = {
        { "this-allocation/which-instances-have-the-prototype-of-their-layout"_s, withEnumeratedClasses<checkWhichInstancesHaveThePrototypeOfTheirLayout> },
#if OS(UNIX)
        { "this-allocation/a-strange-prototype-at-birth-is-stopped-where-the-prototype-stays-as-written"_s, runNativeTestWithLoadedProgram<thisAllocationTests, classThatStaysInSightText, checkStrangePrototypeAtBirthIsStoppedWherePrototypeStaysAsWritten> },
#endif
        { "this-allocation/create-this-fast-paths-create-same-cells"_s, withEnumeratedClasses<checkCreateThisFastPathsCreateSameCells> },
        { "this-allocation/class-below-constructor-function"_s, withEnumeratedClasses<checkClassBelowConstructorFunction>, noClassAboveInRecord },
        { "this-allocation/construction-from-native-code"_s, testConstructionFromNativeCode },
        { "this-allocation/construction-of-one-of-two-subclasses"_s, testConstructionOfOneOfTwoSubclasses },
        { "this-allocation/deferred-closure-defined-by-name"_s, testDeferredClosureDefinedByName },
        { "this-allocation/prototypes-at-even-strides-do-not-stand-in-a-row"_s, testPrototypesAtEvenStridesDoNotStandInARow },
    };
    return tests;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
