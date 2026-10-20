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
#include "AOTCompile.h"
#include "AOTCompileOptionWords.h"
#include "AOTIRParser.h"
#include "AOTImage.h"
#include "AOTKeyOrderTable.h"
#include "AOTLoadedProgramTestSupport.h"
#include "AOTObject.h"
#include "AOTObjectModel.h"
#include "AOTProgramFile.h"
#include "AOTShellProcess.h"
#include "AOTTemporaryFile.h"
#include "AOTThreads.h"
#include "DeferGCInlines.h"
#include "FunctionPrototype.h"
#include "JSCInlines.h"
#include "ObjectConstructor.h"
#include <initializer_list>
#include <stdlib.h>
#include <wtf/DataLog.h>
#include <wtf/Vector.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

std::span<const NativeTest> thisAllocationTests();

static constexpr ASCIILiteral enumeratedClassesText = R"IR(layout L0 kind=Instance name="Base" module=m0 constructor=f1 fields=[{key="x" declared={type=anything}}, {key="label" declared={type=anything}}, {key="word" declared={type=anything}}, {key="list" declared={type=anything}}]
layout L1 kind=Instance name="Derived" module=m0 predictedBase=L0 constructor=f2 fields=[{key="y" declared={type=anything}}] flags=[HasHeritage]
layout L2 kind=Instance name="Unrelated" module=m0 constructor=f3 fields=[{key="u" declared={type=anything}}]
layout L3 kind=Instance name="Point" module=m0 constructor=f4 fields=[{key="px" declared={type=anything}}, {key="py" declared={type=anything}}] flags=[IsConstructorFunction]
layout L4 kind=Instance name="Below" module=m0 predictedBase=L3 constructor=f5 fields=[{key="z" declared={type=anything}}] flags=[HasHeritage]
variable g0 name="Point" module=m0 declared={type=function} initialization=Function function=f4
module m0 path="/main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
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
  b0(v0: anything):
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
  b0(v0: function, v1: anything):
    v2 = GetSuperBase v0 homeKind=Object
    v3 = SuperConstruct v2, v1 layout=L1
    v4 = Integer integer=2
    PropSet v3, v4 key="y" mode=Strict
    Return v3
}
function f3 name="Unrelated" kind=BaseConstructor flags=[HasNewTarget] results=[{type=anything}] {
  b0(v0: anything):
    v1 = GetConstructPrototype v0 layout=L2
    v2 = NewObject v1 layout=L2
    v3 = Integer integer=3
    PropSet v2, v3 key="u" mode=Strict
    Return v2
}
function f4 name="Point" flags=[HasThis] thisType={type=anything predicted=L3} results=[{type=anything}] {
  b0(v0: anything):
    v1 = Integer integer=4
    PropSet v0, v1 key="py" mode=Strict
    v3 = Integer integer=5
    PropSet v0, v3 key="px" mode=Strict
    Return
}
function f5 name="Below" kind=DerivedConstructor flags=[HasNewTarget, TakesCallee] results=[{type=anything}] {
  b0(v0: function, v1: anything):
    v2 = GetSuperBase v0 homeKind=Object
    v3 = SuperConstruct v2, v1 layout=L4
    v4 = Integer integer=6
    PropSet v3, v4 key="z" mode=Strict
    Return v3
}
)IR"_s;

static constexpr ASCIILiteral neverEnumeratedClassesText = R"IR(layout L0 kind=Instance name="Base" module=m0 constructor=f1 fields=[{key="x" declared={type=anything}}, {key="on" declared={type=anything}}]
layout L1 kind=Instance name="Derived" module=m0 predictedBase=L0 constructor=f2 fields=[{key="y" declared={type=anything}}] flags=[HasHeritage]
layout L2 kind=Instance name="Other" module=m0 predictedBase=L0 constructor=f3 fields=[{key="z" declared={type=anything}}] flags=[HasHeritage]
module m0 path="/main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
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
  b0(v0: anything):
    v1 = GetConstructPrototype v0 layout=L0
    v2 = NewObject v1 layout=L0
    v3 = Integer integer=1
    PropSet v2, v3 key="x" mode=Strict
    v5 = Boolean boolean=true
    PropSet v2, v5 key="on" mode=Strict
    Return v2
}
function f2 name="Derived" kind=DerivedConstructor flags=[HasNewTarget, TakesCallee] results=[{type=anything}] {
  b0(v0: function, v1: anything):
    v2 = GetSuperBase v0 homeKind=Object
    v3 = SuperConstruct v2, v1 layout=L1
    v4 = Integer integer=2
    PropSet v3, v4 key="y" mode=Strict
    Return v3
}
function f3 name="Other" kind=DerivedConstructor flags=[HasNewTarget, TakesCallee] results=[{type=anything}] {
  b0(v0: function, v1: anything):
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
  b0():
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
  b0(v0: anything):
    v1 = GetConstructPrototype v0 layout=L0
    v2 = NewObject v1 layout=L0
    v3 = NewClosure v2 function=f2
    PropDefine v2, v3 key="run"
    v5 = Integer integer=7
    PropSet v2, v5 key="x" mode=Strict
    Return v2
}
function f2 name="run" kind=Arrow captures=[{type=L0}] results=[{type=anything}] {
  b0(v0: object ? L0):
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
        if (tables.hasLayoutForShape(shape) && Image::current()->text().equals(StringID(tables.layoutForShape(shape).name), name))
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

static JSObject* prototypeOf(LoadedProgram& program, ASCIILiteral className)
{
    JSObject* prototype = classesOf(program).classPrototype(program.vm(), classNamed(program, className));
    AOT_TEST_CHECK(prototype);
    return prototype;
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
    objects.classes.defineClass(program.realm(), classObject, parent, parent->get(program.realm(), vm.propertyNames->prototype));
    AOT_TEST_CHECK(!vm.exceptionForInspection());
    return classObject;
}

static Object* allocate(LoadedProgram& program, ASCIILiteral className, std::initializer_list<ListedField> listed)
{
    VM& vm = program.vm();
    ObjectModel& objects = *program.state().objects;
    const LayoutData& layout = classNamed(program, className);
    uint32_t orderWord = unorderedOrderWord;
    if (!layout.keysAreNeverEnumerated) {
        Vector<uint16_t> entries;
        for (const ListedField& field : listed)
            entries.append(layout.indexOf(fieldNamed(program, layout, field.name)));
        std::optional<uint32_t> numbered = objects.keyOrders.orderStartingWith(layout, entries.span());
        AOT_TEST_CHECK(numbered);
        orderWord = *numbered;
    }
    Object* object = Object::create(vm, objects.structures.objectStructure(vm, layout.shape), layout, layout.size, orderWord);
    for (const ListedField& field : listed)
        AOT_TEST_CHECK(object->store(vm, fieldNamed(program, layout, field.name), field.value));
    return object;
}

static Object* instanceFor(LoadedProgram& program, Object* allocated, JSObject* prototype)
{
    JSObject* instance = classesOf(program).instanceWithPrototype(program.vm(), allocated, prototype);
    AOT_TEST_CHECK(isCompiledObject(instance));
    return uncheckedDowncast<Object>(instance);
}

static bool isInstanceOf(LoadedProgram& program, JSObject* object, ASCIILiteral className, JSObject* prototype)
{
    if (!isCompiledObject(object) || object->getPrototypeDirect() != JSValue(prototype))
        return false;
    Object* typed = uncheckedDowncast<Object>(object);
    return &typed->layout() == &classNamed(program, className) && !typed->hasFlagsInOrderWord();
}

static String keysOf(LoadedProgram& program, JSObject* object)
{
    AOT_TEST_CHECK(isCompiledObject(object));
    Object* typed = uncheckedDowncast<Object>(object);
    StringBuilder keys;
    typed->forEachFieldInKeyOrder(program.state(), [&](uint16_t index) {
        keys.append(keys.isEmpty() ? ""_s : ","_s, StringView(program.state().objects->layouts.nameOf(program.vm(), typed->layout().fields()[index])));
    });
    return keys.toString();
}

static JSValue valueOf(LoadedProgram& program, JSObject* object, ASCIILiteral name)
{
    return uncheckedDowncast<Object>(object)->getDirectFromNativeCode(program.vm(), Identifier::fromString(program.vm(), name), nullptr);
}

static void storeByName(LoadedProgram& program, JSObject* object, ASCIILiteral name, JSValue value)
{
    PutPropertySlot slot(object, true);
    AOT_TEST_CHECK(object->methodTable()->put(object, program.realm(), Identifier::fromString(program.vm(), name), value, slot));
    AOT_TEST_CHECK(!program.vm().exceptionForInspection());
}

static void checkSubclassInstanceHasNoFieldTheAllocationLeftOut(LoadedProgram& program)
{
    DeferGC deferGC(program.vm());
    JSObject* prototype = prototypeOf(program, "Derived"_s);
    Object* allocated = allocate(program, "Base"_s, { });
    Object* instance = instanceFor(program, allocated, prototype);

    AOT_TEST_CHECK(instance != allocated);
    AOT_TEST_CHECK(isInstanceOf(program, instance, "Derived"_s, prototype));
    AOT_TEST_CHECK(keysOf(program, instance).isEmpty());
    for (ASCIILiteral name : { "x"_s, "label"_s, "word"_s, "list"_s, "y"_s })
        AOT_TEST_CHECK(!valueOf(program, instance, name));
}

static void checkSubclassInstanceGetsTheListedFields(LoadedProgram& program)
{
    VM& vm = program.vm();
    DeferGC deferGC(vm);
    JSObject* prototype = prototypeOf(program, "Derived"_s);
    JSValue label = jsNontrivialString(vm, "text"_s);
    Object* instance = instanceFor(program, allocate(program, "Base"_s, { { "label"_s, label }, { "x"_s, jsNumber(1) } }), prototype);

    AOT_TEST_CHECK(isInstanceOf(program, instance, "Derived"_s, prototype));
    AOT_TEST_CHECK(keysOf(program, instance) == "label,x"_s);
    AOT_TEST_CHECK(valueOf(program, instance, "label"_s) == label && valueOf(program, instance, "x"_s) == jsNumber(1) && !valueOf(program, instance, "y"_s));

    Object* withOneField = instanceFor(program, allocate(program, "Base"_s, { { "x"_s, jsNumber(1) } }), prototype);
    AOT_TEST_CHECK(isInstanceOf(program, withOneField, "Derived"_s, prototype));
    AOT_TEST_CHECK(keysOf(program, withOneField) == "x"_s && !valueOf(program, withOneField, "label"_s));
}

static void checkKeysAreInCreationOrder(LoadedProgram& program)
{
    DeferGC deferGC(program.vm());
    const LayoutData& derived = classNamed(program, "Derived"_s);
    AOT_TEST_CHECK(!derived.keysAreNeverEnumerated);
    AOT_TEST_CHECK(derived.indexOf(fieldNamed(program, derived, "y"_s)) < derived.indexOf(fieldNamed(program, derived, "x"_s)));
    JSObject* prototype = prototypeOf(program, "Derived"_s);

    Object* baseFirst = instanceFor(program, allocate(program, "Base"_s, { }), prototype);
    storeByName(program, baseFirst, "x"_s, jsNumber(1));
    storeByName(program, baseFirst, "label"_s, jsNull());
    storeByName(program, baseFirst, "y"_s, jsNumber(2));
    AOT_TEST_CHECK(keysOf(program, baseFirst) == "x,label,y"_s);
    storeByName(program, baseFirst, "x"_s, jsNumber(1));
    storeByName(program, baseFirst, "label"_s, jsUndefined());
    AOT_TEST_CHECK(keysOf(program, baseFirst) == "x,label,y"_s);
    AOT_TEST_CHECK(isInstanceOf(program, baseFirst, "Derived"_s, prototype));

    Object* ownFirst = instanceFor(program, allocate(program, "Base"_s, { }), prototype);
    storeByName(program, ownFirst, "y"_s, jsNumber(2));
    storeByName(program, ownFirst, "x"_s, jsNumber(1));
    AOT_TEST_CHECK(keysOf(program, ownFirst) == "y,x"_s);

    Object* afterListedField = instanceFor(program, allocate(program, "Base"_s, { { "label"_s, jsNull() } }), prototype);
    storeByName(program, afterListedField, "y"_s, jsNumber(2));
    storeByName(program, afterListedField, "x"_s, jsNumber(1));
    AOT_TEST_CHECK(keysOf(program, afterListedField) == "label,y,x"_s);
}

static void checkPrototypesOfNoOtherClassKeepTheAllocatedObject(LoadedProgram& program)
{
    DeferGC deferGC(program.vm());
    JSObject* prototypeOfConstructorFunction = asObject(constructorOf(program, "Point"_s)->get(program.realm(), program.vm().propertyNames->prototype));
    for (JSObject* prototype : { prototypeOf(program, "Base"_s), prototypeOfConstructorFunction, static_cast<JSObject*>(constructEmptyObject(program.realm())) }) {
        Object* allocated = allocate(program, "Base"_s, { { "x"_s, jsNumber(1) } });
        AOT_TEST_CHECK(instanceFor(program, allocated, prototype) == allocated);
        AOT_TEST_CHECK(isInstanceOf(program, allocated, "Base"_s, prototype));
        AOT_TEST_CHECK(keysOf(program, allocated) == "x"_s && valueOf(program, allocated, "x"_s) == jsNumber(1));
    }
}

// Nothing that has the shape of the allocated class belongs to another class. The constructor of a class that is not below the allocated one does not run, so its shape is not given either.
static void checkPrototypeOfUnrelatedClassGivesOrdinaryObject(LoadedProgram& program)
{
    VM& vm = program.vm();
    DeferGC deferGC(vm);
    JSObject* unrelatedPrototype = prototypeOf(program, "Unrelated"_s);
    JSObject* forUnrelatedClass = classesOf(program).instanceWithPrototype(vm, allocate(program, "Base"_s, { }), unrelatedPrototype);
    AOT_TEST_CHECK(!isCompiledObject(forUnrelatedClass) && forUnrelatedClass->getPrototypeDirect() == JSValue(unrelatedPrototype));

    JSObject* basePrototype = prototypeOf(program, "Base"_s);
    JSObject* forBaseClass = classesOf(program).instanceWithPrototype(vm, allocate(program, "Derived"_s, { }), basePrototype);
    AOT_TEST_CHECK(!isCompiledObject(forBaseClass) && forBaseClass->getPrototypeDirect() == JSValue(basePrototype));
}

// HasLayoutPrototype reads the flag of the Structure, which speaks of the layout the object has, not of the layout the site allocated.
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

static void checkCreateThisStartsWithoutKeys(LoadedProgram& program)
{
    DeferGC deferGC(program.vm());
    const LayoutData& point = classNamed(program, "Point"_s);
    AOT_TEST_CHECK(!point.keysAreNeverEnumerated);
    AOT_TEST_CHECK(point.indexOf(fieldNamed(program, point, "px"_s)) < point.indexOf(fieldNamed(program, point, "py"_s)));
    JSObject* constructor = constructorOf(program, "Point"_s);
    JSObject* prototype = asObject(constructor->get(program.realm(), program.vm().propertyNames->prototype));

    constexpr unsigned numberOfCallsThatReachTheRememberedAllocation = 3;
    for (unsigned call = 0; call < numberOfCallsThatReachTheRememberedAllocation; ++call) {
        JSObject* object = classesOf(program).createThis(program.realm(), constructor, constructor);
        AOT_TEST_CHECK(isInstanceOf(program, object, "Point"_s, prototype));
        AOT_TEST_CHECK(keysOf(program, object).isEmpty() && !valueOf(program, object, "px"_s) && !valueOf(program, object, "py"_s));
        storeByName(program, object, "py"_s, jsNumber(4));
        storeByName(program, object, "px"_s, jsNumber(5));
        AOT_TEST_CHECK(keysOf(program, object) == "py,px"_s);
    }

    JSObject* forUnrelatedClass = classesOf(program).createThis(program.realm(), constructor, constructorOf(program, "Unrelated"_s));
    AOT_TEST_CHECK(!isCompiledObject(forUnrelatedClass) && forUnrelatedClass->getPrototypeDirect() == JSValue(prototypeOf(program, "Unrelated"_s)));

    JSObject* forNativeFunction = classesOf(program).createThis(program.realm(), constructor, program.realm()->objectConstructor());
    AOT_TEST_CHECK(!isCompiledObject(forNativeFunction) && forNativeFunction->getPrototypeDirect() == JSValue(program.realm()->objectPrototype()));

    JSObject* ofSubclass = classesOf(program).createThis(program.realm(), constructor, defineClass(program, "Below"_s, constructor));
    AOT_TEST_CHECK(isInstanceOf(program, ofSubclass, "Below"_s, prototypeOf(program, "Below"_s)));
    AOT_TEST_CHECK(keysOf(program, ofSubclass).isEmpty());
    storeByName(program, ofSubclass, "py"_s, jsNumber(4));
    storeByName(program, ofSubclass, "px"_s, jsNumber(5));
    storeByName(program, ofSubclass, "z"_s, jsNumber(6));
    AOT_TEST_CHECK(keysOf(program, ofSubclass) == "py,px,z"_s);
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

static void checkPlaceholderIsNotDefinedOnTheInstance(LoadedProgram& program)
{
    DeferGC deferGC(program.vm());
    const RuntimeLayoutTable& layouts = program.state().objects->layouts;
    AOT_TEST_CHECK(classNamed(program, "Base"_s).keysAreNeverEnumerated);
    for (ASCIILiteral className : { "Base"_s, "Derived"_s, "Other"_s }) {
        const LayoutData& layout = classNamed(program, className);
        AOT_TEST_CHECK(!layout.extraPropertiesSlot());
        AOT_TEST_CHECK(!fieldNamed(program, layout, "x"_s).canLeaveSlot() && !layouts.fieldAccepts(fieldNamed(program, layout, "x"_s), jsNumber(0)));
        AOT_TEST_CHECK(!fieldNamed(program, layout, "on"_s).canLeaveSlot() && !layouts.fieldAccepts(fieldNamed(program, layout, "on"_s), jsBoolean(false)));
    }

    for (ASCIILiteral className : { "Derived"_s, "Other"_s }) {
        JSObject* prototype = prototypeOf(program, className);
        Object* allocated = allocate(program, "Base"_s, { });
        Object* instance = instanceFor(program, allocated, prototype);
        AOT_TEST_CHECK(instance != allocated && isInstanceOf(program, instance, className, prototype));

        Object* filled = instanceFor(program, allocate(program, "Base"_s, { { "x"_s, jsNumber(1) }, { "on"_s, jsBoolean(true) } }), prototype);
        AOT_TEST_CHECK(isInstanceOf(program, filled, className, prototype));
        AOT_TEST_CHECK(valueOf(program, filled, "x"_s) == jsNumber(1) && valueOf(program, filled, "on"_s) == jsBoolean(true));
    }
}

static void checkOneStoreByNameFillsSlotWithoutAbsentState(LoadedProgram& program)
{
    VM& vm = program.vm();
    DeferGC deferGC(vm);
    const FieldData& field = fieldNamed(program, classNamed(program, "Derived"_s), "y"_s);
    AOT_TEST_CHECK(field.flags & fieldIsStoredOnce && !field.canLeaveSlot());
    JSObject* prototype = prototypeOf(program, "Derived"_s);
    Object* instance = instanceFor(program, allocate(program, "Base"_s, { }), prototype);

    AOT_TEST_CHECK(!instance->tryStore(vm, field, jsNumber(3)) && !instance->tryStore(vm, field, jsUndefined()));
    storeByName(program, instance, "y"_s, jsNumber(2));
    AOT_TEST_CHECK(isInstanceOf(program, instance, "Derived"_s, prototype));
    AOT_TEST_CHECK(valueOf(program, instance, "y"_s) == jsNumber(2));
}

static std::unique_ptr<CompileOutput> compileOnThisThread(const ShellRun& run, std::initializer_list<ASCIILiteral> optionWords)
{
    Wire::CompileOptions options = makeDefaultCompileOptions();
    if (run.scriptUsesWhatTheProgramCreates)
        AOT_TEST_CHECK(!applyOptionWord(options, "admit=RuntimeCode"_s));
    for (ASCIILiteral word : optionWords)
        AOT_TEST_CHECK(!applyOptionWord(options, word));
    BuildContext build(options);
    std::unique_ptr<Typed::Program> program = Typed::parseProgram(Typed::withStandInLibrary(String(run.programText)), Typed::IRForm::AsRead, build);
    if (!program) {
        build.diagnostics().finish(nullptr);
        Typed::printDiagnostics(build.diagnostics(), WTF::dataFile());
    }
    AOT_TEST_CHECK(program);

    std::unique_ptr<CompileOutput> output = compileParsedProgram(WTF::move(program), build);
    if (output->image.isEmpty())
        Typed::printDiagnostics(output->diagnostics, WTF::dataFile());
    AOT_TEST_CHECK(!output->image.isEmpty());
    AOT_TEST_CHECK(output->entryModuleIDs.size() == 1);
    return output;
}

static void checkOutputInShell(const ShellRun& run, std::initializer_list<ASCIILiteral> optionWords)
{
    std::unique_ptr<CompileOutput> compiled;
    runOnBuildThread([&] {
        compiled = compileOnThisThread(run, optionWords);
    });

    TemporaryFile image(".image"_s, compiled->image.span());
    Vector<String> arguments {
        makeString("--aotImagePath="_s, image.path()),
        String("-e"_s),
        makeString(run.script, "runCompiledEntryPoint("_s, compiled->entryModuleIDs[0], ").then(() => {"_s, run.statementsAfterProgram, "}).catch(error => print(\"the script threw \" + error));"_s),
    };
    String shell = shellPath();
    ChildProcessResult result = ChildProcessLauncher(String(shell), { }).run(arguments.span());
    String output = makeStringByReplacingAll(result.output, "\r\n"_s, "\n"_s);
    if (!result.exitedWith(EXIT_SUCCESS) || output != run.expectedOutput)
        dataLogLn(shell, " ", result.description(), "\n", output);
    AOT_TEST_CHECK(result.exitedWith(EXIT_SUCCESS));
    AOT_TEST_CHECK(output == run.expectedOutput);
}

static void checkOutputInShellWithAndWithoutPasses(const ShellRun& run)
{
    checkOutputInShell(run, { });
    checkOutputInShell(run, { "validate"_s });
    checkOutputInShell(run, { "references=narrow"_s });
    checkOutputInShell(run, { "disable=pass:InlineCalls"_s });
    checkOutputInShell(run, { "disable=pass:SinkAllocations"_s });
    checkOutputInShell(run, { "only-passes=ResolveAccesses"_s });
    checkOutputInShell(run, { "disable=pass:InlineCalls,pass:SinkAllocations"_s, "references=narrow"_s, "validate"_s });
}

static void testConstructionFromNativeCode()
{
    checkOutputInShellWithAndWithoutPasses({ enumeratedClassesText, scriptForEnumeratedClasses, statementsAfterEnumeratedClasses, outputOfEnumeratedClasses, true });
}

static void testConstructionOfOneOfTwoSubclasses()
{
    checkOutputInShellWithAndWithoutPasses({ neverEnumeratedClassesText, scriptForNeverEnumeratedClasses, ""_s, outputOfNeverEnumeratedClasses });
}

static void testDeferredClosureDefinedByName()
{
    ShellRun run { closureOverInstanceText, ""_s, ""_s, "7 true\n"_s };
    checkOutputInShell(run, { });
    checkOutputInShell(run, { "references=narrow"_s });
    checkOutputInShell(run, { "references=narrow"_s, "validate"_s });
    checkOutputInShell(run, { "references=narrow"_s, "only-passes=ResolveAccesses"_s, "validate"_s });
    checkOutputInShell(run, { "references=narrow"_s, "disable=pass:ResolveAccesses"_s });
    checkOutputInShell(run, { "references=narrow"_s, "disable=pass:ResolveAccesses"_s, "validate"_s });
}

template<LoadedProgramTest body>
static constexpr void (*withEnumeratedClasses)() = runNativeTestWithLoadedProgram<thisAllocationTests, enumeratedClassesText, body>;

template<LoadedProgramTest body>
static constexpr void (*withNeverEnumeratedClasses)() = runNativeTestWithLoadedProgram<thisAllocationTests, neverEnumeratedClassesText, body>;

std::span<const NativeTest> thisAllocationTests()
{
    static constexpr NativeTest tests[] = {
        { "this-allocation/subclass-instance-has-no-field-the-allocation-left-out"_s, withEnumeratedClasses<checkSubclassInstanceHasNoFieldTheAllocationLeftOut> },
        { "this-allocation/subclass-instance-gets-the-listed-fields"_s, withEnumeratedClasses<checkSubclassInstanceGetsTheListedFields> },
        { "this-allocation/keys-are-in-creation-order"_s, withEnumeratedClasses<checkKeysAreInCreationOrder> },
        { "this-allocation/prototypes-of-no-other-class-keep-the-allocated-object"_s, withEnumeratedClasses<checkPrototypesOfNoOtherClassKeepTheAllocatedObject> },
        { "this-allocation/prototype-of-unrelated-class-gives-ordinary-object"_s, withEnumeratedClasses<checkPrototypeOfUnrelatedClassGivesOrdinaryObject> },
        { "this-allocation/which-instances-have-the-prototype-of-their-layout"_s, withEnumeratedClasses<checkWhichInstancesHaveThePrototypeOfTheirLayout> },
        { "this-allocation/create-this-starts-without-keys"_s, withEnumeratedClasses<checkCreateThisStartsWithoutKeys> },
        { "this-allocation/class-below-constructor-function"_s, withEnumeratedClasses<checkClassBelowConstructorFunction> },
        { "this-allocation/placeholder-is-not-defined-on-the-instance"_s, withNeverEnumeratedClasses<checkPlaceholderIsNotDefinedOnTheInstance> },
        { "this-allocation/one-store-by-name-fills-slot-without-absent-state"_s, withNeverEnumeratedClasses<checkOneStoreByNameFillsSlotWithoutAbsentState> },
        { "this-allocation/construction-from-native-code"_s, testConstructionFromNativeCode },
        { "this-allocation/construction-of-one-of-two-subclasses"_s, testConstructionOfOneOfTwoSubclasses },
        { "this-allocation/deferred-closure-defined-by-name"_s, testDeferredClosureDefinedByName },
    };
    return tests;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
