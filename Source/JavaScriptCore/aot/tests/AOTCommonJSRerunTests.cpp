/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTLoadedProgramTestSupport.h"
#include "AOTModuleHost.h"
#include "AOTNativeTest.h"
#include "AOTProgramTables.h"
#include "JSCInlines.h"
#include "TopExceptionScope.h"

namespace JSC { namespace AOT {

std::span<const NativeTest> commonJSRerunTests();

static constexpr ASCIILiteral programWithoutEscape = R"IR(module m0 path="/app/throws.cjs" displayPath="throws.cjs" kind=CommonJs exportRule=CommonJs requireResult=ModuleExports body=f0
module m1 path="/app/main.cjs" displayPath="main.cjs" kind=CommonJs exportRule=CommonJs requireResult=ModuleExports body=f7
entry module=m1
layout L0 kind=Environment name="throws" fields=[{key=@"count.0" declared={type=anything|absent}}, {key=@"late.1" declared={type=anything|absent}}, {key=@"Point.2" declared={type=anything|absent}}, {key=@"tag.3" declared={type=anything|absent}}]
layout L1 kind=Instance name="Point" constructor=f4 fields=[{key="x" declared={type=anything}}] staticFields=[{key="made" variable=g0}]
variable g0 module=m0 declared={type=anything} owner=L1

function f0 name="throws" kind=CommonJsModule flags=[HasThis] thisType={type=anything} parameters=[{type=anything}, {type=anything}, {type=anything}, {type=anything}, {type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything, v2: anything, v3: anything, v4: anything, v5: anything):
    v6 = Absent
    v7 = NewObject -, v6, v6, v6, v6 layout=L0 fields=[0, 1, 2, 3]
    v8 = NewClosure v7 function=f1
    v9 = NewClosure v7 function=f2
    v10 = NewClosure v7 function=f3
    v11 = Integer integer=0
    PutEnvironmentField v7, v11 layout=L0 field=0
    v13 = Builtin builtin=GlobalThis
    v14 = PropGet v13 key="atStart" [isFreeName]
    v15 = Call v14
    Jump -> next b1()
  b1() handler=b3():
    v16 = Call v8
    v17 = PropGet v13 key="sawValue" [isFreeName]
    v18 = Call v17
    Jump -> next b2()
  b2():
    v19 = Integer integer=5
    PutEnvironmentField v7, v19 layout=L0 field=1
    v21 = Call v9
    v22 = Builtin builtin=Symbol
    v23 = String string="tag"
    v24 = Call v22, -, v23 @100
    PutEnvironmentField v7, v24 layout=L0 field=3
    v26 = NewClosure v7 function=f4
    v27 = NewClass v26 layout=L1
    v28 = NewClosure function=f5
    DefineMethod v27, v28 layout=L1 side=Prototype key="getX"
    v30 = FinishClass v27 layout=L1
    v31 = CallDirect v30 function=f6
    PutEnvironmentField v7, v30 layout=L0 field=2
    v33 = Integer integer=7
    v34 = Construct v30, -, v33
    v35 = PropGet v30 key="made"
    v36 = Integer integer=1
    v37 = Add v35, v36
    PropSet v30, v37 key="made" mode=Strict
    v39 = PropGet v13 key="shouldFail" [isFreeName]
    v40 = Call v39
    v41 = ToBoolean v40
    Branch v41 -> taken b4(), notTaken b5()
  b3() role=CatchHandler:
    v42 = CatchValue
    v43 = PropGet v13 key="sawDeadZone" [isFreeName]
    v44 = Call v43
    Jump -> next b2()
  b4():
    v45 = String string="left by the failed run"
    PropSet v30, v45 key="extra" mode=Strict
    v47 = PropGet v30 key="prototype"
    v48 = PropGet v47 key="getX"
    PropSet v48, v45 key="mark" mode=Strict
    v50 = String string="failed"
    Throw v50
  b5():
    v51 = PropGet v13 key="publish" [isFreeName]
    v52 = Call v51, -, v9, v8, v34, v30, v24, v10
    Return
}
function f1 name="early" captures=[{type=env L0}] results=[{type=anything}] {
  b0(v0: env L0):
    v1 = GetEnvironmentField v0 layout=L0 field=1
    v2 = CheckInitialized v1 name="late"
    Return v2
}
function f2 name="next" captures=[{type=env L0}] results=[{type=anything}] {
  b0(v0: env L0):
    v1 = GetEnvironmentField v0 layout=L0 field=0
    v2 = CheckInitialized v1 name="count"
    v3 = Integer integer=1
    v4 = Add v2, v3
    PutEnvironmentField v0, v4 layout=L0 field=0
    Return v4
}
function f3 name="inspect" captures=[{type=env L0}] parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: env L0, v1: anything):
    v2 = GetEnvironmentField v0 layout=L0 field=2
    v3 = CheckInitialized v2 name="Point"
    v4 = InstanceOf v1, v3
    Branch v4 -> taken b1(), notTaken b2()
  b1():
    v5 = PropGet v1 key="getX"
    v6 = Call v5, v1
    Return v6
  b2():
    v7 = String string="not a Point"
    Return v7
}
function f4 name="Point" kind=BaseConstructor flags=[HasNewTarget] captures=[{type=env L0}] parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: env L0, v1: anything, v2: anything):
    v3 = GetConstructPrototype v1 layout=L1
    v4 = NewObject v3 layout=L1 fields=[]
    PropDefine v4, v2 key="x"
    v6 = GetEnvironmentField v0 layout=L0 field=3
    v7 = CheckInitialized v6 name="tag"
    v8 = String string="tagged"
    ElemSet v4, v7, v8 mode=Strict
    Return v4
}
function f5 name="getX" kind=Method flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
  b0(v0: anything):
    v1 = PropGet v0 key="x"
    Return v1
}
function f6 name="initializePoint" kind=Initializer flags=[HasThis, IsHiddenInStackTraces] thisType={type=function} results=[{type=anything}] {
  b0(v0: function):
    v1 = Integer integer=0
    PropDefine v0, v1 key="made"
    v3 = Undefined
    Return v3
}
function f7 name="main" module=m1 kind=CommonJsModule flags=[HasThis] thisType={type=anything} parameters=[{type=anything}, {type=anything}, {type=anything}, {type=anything}, {type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything, v2: anything, v3: anything, v4: anything, v5: anything):
    v6 = RequireModule module=m0 referrer=m1 specifier="./throws.cjs"
    Return
}
)IR"_s;

static constexpr ASCIILiteral programWithEscape = R"IR(module m0 path="/app/throws.cjs" displayPath="throws.cjs" kind=CommonJs exportRule=CommonJs requireResult=ModuleExports body=f0
module m1 path="/app/main.cjs" displayPath="main.cjs" kind=CommonJs exportRule=CommonJs requireResult=ModuleExports body=f7
entry module=m1
layout L0 kind=Environment name="throws" fields=[{key=@"count.0" declared={type=anything|absent}}, {key=@"late.1" declared={type=anything|absent}}, {key=@"Point.2" declared={type=anything|absent}}, {key=@"tag.3" declared={type=anything|absent}}]
layout L1 kind=Instance name="Point" constructor=f4 fields=[{key="x" declared={type=anything}}] staticFields=[{key="made" variable=g0}]
variable g0 module=m0 declared={type=anything} owner=L1

function f0 name="throws" kind=CommonJsModule flags=[HasThis] thisType={type=anything} parameters=[{type=anything}, {type=anything}, {type=anything}, {type=anything}, {type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything, v2: anything, v3: anything, v4: anything, v5: anything):
    v6 = Absent
    v7 = NewObject -, v6, v6, v6, v6 layout=L0 fields=[0, 1, 2, 3]
    v8 = NewClosure v7 function=f1
    v9 = NewClosure v7 function=f2
    v10 = NewClosure v7 function=f3
    v11 = Integer integer=0
    PutEnvironmentField v7, v11 layout=L0 field=0
    v13 = Builtin builtin=GlobalThis
    v14 = PropGet v13 key="atStart" [isFreeName]
    v15 = Call v14
    Jump -> next b1()
  b1() handler=b3():
    v16 = Call v8
    v17 = PropGet v13 key="sawValue" [isFreeName]
    v18 = Call v17
    Jump -> next b2()
  b2():
    v19 = Integer integer=5
    PutEnvironmentField v7, v19 layout=L0 field=1
    v21 = Call v9
    v22 = Builtin builtin=Symbol
    v23 = String string="tag"
    v24 = Call v22, -, v23 @100
    PutEnvironmentField v7, v24 layout=L0 field=3
    v26 = NewClosure v7 function=f4
    v27 = NewClass v26 layout=L1
    v28 = NewClosure function=f5
    DefineMethod v27, v28 layout=L1 side=Prototype key="getX"
    v30 = FinishClass v27 layout=L1
    v31 = CallDirect v30 function=f6
    PutEnvironmentField v7, v30 layout=L0 field=2
    v33 = Integer integer=7
    v34 = Construct v30, -, v33
    v35 = PropGet v30 key="made"
    v36 = Integer integer=1
    v37 = Add v35, v36
    PropSet v30, v37 key="made" mode=Strict
    v39 = PropGet v13 key="shouldFail" [isFreeName]
    v40 = Call v39
    v41 = ToBoolean v40
    Branch v41 -> taken b4(), notTaken b5()
  b3() role=CatchHandler:
    v42 = CatchValue
    v43 = PropGet v13 key="sawDeadZone" [isFreeName]
    v44 = Call v43
    Jump -> next b2()
  b4():
    v45 = String string="left by the failed run"
    PropSet v30, v45 key="extra" mode=Strict
    v47 = PropGet v30 key="prototype"
    v48 = PropGet v47 key="getX"
    PropSet v48, v45 key="mark" mode=Strict
    v50 = PropGet v13 key="keep" [isFreeName]
    v51 = Call v50, -, v9
    v52 = Call v50, -, v34
    v53 = Call v50, -, v30
    v54 = Call v50, -, v24
    v55 = String string="failed"
    Throw v55
  b5():
    v56 = PropGet v13 key="publish" [isFreeName]
    v57 = Call v56, -, v9, v8, v34, v30, v24, v10
    Return
}
function f1 name="early" captures=[{type=env L0}] results=[{type=anything}] {
  b0(v0: env L0):
    v1 = GetEnvironmentField v0 layout=L0 field=1
    v2 = CheckInitialized v1 name="late"
    Return v2
}
function f2 name="next" captures=[{type=env L0}] results=[{type=anything}] {
  b0(v0: env L0):
    v1 = GetEnvironmentField v0 layout=L0 field=0
    v2 = CheckInitialized v1 name="count"
    v3 = Integer integer=1
    v4 = Add v2, v3
    PutEnvironmentField v0, v4 layout=L0 field=0
    Return v4
}
function f3 name="inspect" captures=[{type=env L0}] parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: env L0, v1: anything):
    v2 = GetEnvironmentField v0 layout=L0 field=2
    v3 = CheckInitialized v2 name="Point"
    v4 = InstanceOf v1, v3
    Branch v4 -> taken b1(), notTaken b2()
  b1():
    v5 = PropGet v1 key="getX"
    v6 = Call v5, v1
    Return v6
  b2():
    v7 = String string="not a Point"
    Return v7
}
function f4 name="Point" kind=BaseConstructor flags=[HasNewTarget] captures=[{type=env L0}] parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: env L0, v1: anything, v2: anything):
    v3 = GetConstructPrototype v1 layout=L1
    v4 = NewObject v3 layout=L1 fields=[]
    PropDefine v4, v2 key="x"
    v6 = GetEnvironmentField v0 layout=L0 field=3
    v7 = CheckInitialized v6 name="tag"
    v8 = String string="tagged"
    ElemSet v4, v7, v8 mode=Strict
    Return v4
}
function f5 name="getX" kind=Method flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
  b0(v0: anything):
    v1 = PropGet v0 key="x"
    Return v1
}
function f6 name="initializePoint" kind=Initializer flags=[HasThis, IsHiddenInStackTraces] thisType={type=function} results=[{type=anything}] {
  b0(v0: function):
    v1 = Integer integer=0
    PropDefine v0, v1 key="made"
    v3 = Undefined
    Return v3
}
function f7 name="main" module=m1 kind=CommonJsModule flags=[HasThis] thisType={type=anything} parameters=[{type=anything}, {type=anything}, {type=anything}, {type=anything}, {type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything, v2: anything, v3: anything, v4: anything, v5: anything):
    v6 = RequireModule module=m0 referrer=m1 specifier="./throws.cjs"
    Return
}
)IR"_s;

static constexpr ASCIILiteral programWithClassHierarchy = R"IR(module m0 path="/app/throws.cjs" displayPath="throws.cjs" kind=CommonJs exportRule=CommonJs requireResult=ModuleExports body=f0
module m1 path="/app/main.cjs" displayPath="main.cjs" kind=CommonJs exportRule=CommonJs requireResult=ModuleExports body=f7
entry module=m1
layout L0 kind=Instance name="Base" constructor=f1 fields=[{key="x" declared={type=anything}}] staticFields=[{key="label" variable=g0}]
layout L1 kind=Instance name="Derived" predictedBase=L0 constructor=f4 flags=[HasHeritage]
variable g0 module=m0 declared={type=anything} owner=L0

function f0 name="throws" kind=CommonJsModule flags=[HasThis] thisType={type=anything} parameters=[{type=anything}, {type=anything}, {type=anything}, {type=anything}, {type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything, v2: anything, v3: anything, v4: anything, v5: anything):
    v6 = Builtin builtin=GlobalThis
    v7 = PropGet v6 key="atStart" [isFreeName]
    v8 = Call v7
    v9 = NewClosure function=f1
    v10 = NewClass v9 layout=L0
    v11 = NewClosure function=f2
    DefineMethod v10, v11 layout=L0 side=Prototype key="getX"
    v13 = NewClosure function=f3
    DefineMethod v10, v13 layout=L0 side=Static index=1 key="kind"
    v15 = FinishClass v10 layout=L0
    v16 = CallDirect v15 function=f6
    v17 = ResolveHeritage v15
    v18 = NewClosure function=f4
    v19 = NewClass v18, v15, v17 layout=L1
    v20 = NewClosure function=f5
    DefineMethod v19, v20 layout=L1 side=Prototype key="readX"
    v22 = FinishClass v19 layout=L1
    v23 = Integer integer=3
    v24 = Construct v22, -, v23
    v25 = PropGet v6 key="shouldFail" [isFreeName]
    v26 = Call v25
    v27 = ToBoolean v26
    Branch v27 -> taken b1(), notTaken b2()
  b1():
    v28 = PropGet v6 key="keep" [isFreeName]
    v29 = Call v28, -, v15
    v30 = Call v28, -, v22
    v31 = Call v28, -, v24
    v32 = String string="failed"
    Throw v32
  b2():
    v33 = PropGet v6 key="publishClasses" [isFreeName]
    v34 = Call v33, -, v15, v22, v24
    Return
}
function f1 name="Base" kind=BaseConstructor flags=[HasNewTarget] parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything):
    v2 = GetConstructPrototype v0 layout=L0
    v3 = NewObject v2 layout=L0 fields=[]
    PropDefine v3, v1 key="x"
    Return v3
}
function f2 name="getX" kind=Method flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
  b0(v0: anything):
    v1 = PropGet v0 key="x"
    Return v1
}
function f3 name="kind" kind=Method flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
  b0(v0: anything):
    v1 = String string="a base"
    Return v1
}
function f4 name="Derived" kind=DerivedConstructor flags=[HasNewTarget, TakesCallee] parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: function, v1: anything, v2: anything):
    v3 = GetSuperBase v0
    v4 = SuperConstruct v3, v1, v2 layout=L1
    Return v4
}
function f5 name="readX" kind=Method flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
  b0(v0: anything):
    v1 = PropGet v0 key="getX"
    v2 = Call v1, v0
    Return v2
}
function f6 name="initializeBase" kind=Initializer flags=[HasThis, IsHiddenInStackTraces] thisType={type=function} results=[{type=anything}] {
  b0(v0: function):
    v1 = String string="base"
    PropDefine v0, v1 key="label"
    v3 = Undefined
    Return v3
}
function f7 name="main" module=m1 kind=CommonJsModule flags=[HasThis] thisType={type=anything} parameters=[{type=anything}, {type=anything}, {type=anything}, {type=anything}, {type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything, v2: anything, v3: anything, v4: anything, v5: anything):
    v6 = RequireModule module=m0 referrer=m1 specifier="./throws.cjs"
    Return
}
)IR"_s;

static constexpr ModuleID throwingModule { 0u };

static void defineHostFunctions(LoadedProgram& program)
{
    program.realms().evaluate(program.realm(), R"JS(
        var fail = true;
        var kept = [];
        var deadZones = 0;
        var values = 0;
        function shouldFail() { return fail; }
        function keep(value) { kept[kept.length] = value; return value; }
        function sawDeadZone() { ++deadZones; }
        function sawValue() { ++values; }
        var atStart = function () { };
        var loaded;
        function publish(next, early, point, Point, tag, inspect) { loaded = { next, early, point, Point, tag, inspect }; }
        function publishClasses(Base, Derived, object) { loaded = { Base, Derived, object }; }
    )JS"_s);
}

static void check(LoadedProgram& program, ASCIILiteral expression)
{
    AOT_TEST_CHECK_FOR_SOURCE(program.realms().evaluate(program.realm(), expression) == jsBoolean(true), expression);
}

static void requireBodyThatThrows(LoadedProgram& program)
{
    VM& vm = program.vm();
    JSGlobalObject* realm = program.realm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);

    JSValue result = requireModule(realm, throwingModule, createCommonJSModuleOfTestHost(realm));

    AOT_TEST_CHECK(!result && scope.exception());
    JSValue thrown = scope.exception()->value();
    scope.clearException();
    AOT_TEST_CHECK(thrown.isString() && asString(thrown)->value(realm).data == "failed"_s);
    AOT_TEST_CHECK(!isEvaluated(realm, throwingModule));
}

static void requireBodyThatCompletes(LoadedProgram& program)
{
    VM& vm = program.vm();
    JSGlobalObject* realm = program.realm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    program.realms().evaluate(realm, "fail = false"_s);

    JSValue exports = requireModule(realm, throwingModule, createCommonJSModuleOfTestHost(realm));

    AOT_TEST_CHECK(!scope.exception() && exports.isObject() && isEvaluated(realm, throwingModule));
}

static void checkRunThatCompleted(LoadedProgram& program)
{
    check(program, "deadZones === 2 && values === 0"_s);
    check(program, "loaded.next() === 2"_s);
    check(program, "loaded.early() === 5"_s);
    check(program, "loaded.point.getX() === 7"_s);
    check(program, "loaded.inspect(loaded.point) === 7"_s);
    check(program, "loaded.Point.made === 1"_s);
    check(program, "loaded.Point.extra === undefined && !('extra' in loaded.Point)"_s);
    check(program, "loaded.point.getX.mark === undefined"_s);
    check(program, "loaded.point instanceof loaded.Point"_s);
    check(program, "Object.getPrototypeOf(loaded.point) === loaded.Point.prototype"_s);
    check(program, "loaded.Point.prototype.constructor === loaded.Point"_s);
    check(program, "loaded.point[loaded.tag] === 'tagged'"_s);
    check(program, "new loaded.Point(8).getX() === 8"_s);
}

static void checkTablesListCellsOfModuleObjects(LoadedProgram& program)
{
    const ProgramTables& tables = program.state().tables;
    for (const ModuleResetSlotData& reset : tables.moduleResetSlots()) {
        AOT_TEST_CHECK(reset.slot.representation() == SlotRepresentation::JSValue);
    }
}

static void checkSecondRunStartsAfresh(LoadedProgram& program)
{
    defineHostFunctions(program);
    requireBodyThatThrows(program);
    requireBodyThatCompletes(program);
    checkRunThatCompleted(program);
}

static void checkThirdRunStartsAfresh(LoadedProgram& program)
{
    defineHostFunctions(program);
    requireBodyThatThrows(program);
    requireBodyThatThrows(program);
    requireBodyThatCompletes(program);
    check(program, "deadZones === 3 && values === 0"_s);
    check(program, "loaded.next() === 2 && loaded.Point.made === 1 && loaded.Point.extra === undefined"_s);
    check(program, "Object.getPrototypeOf(loaded.point) === loaded.Point.prototype"_s);
}

static void checkWhatEscapedStaysUsable(LoadedProgram& program)
{
    defineHostFunctions(program);
    requireBodyThatThrows(program);
    check(program, "kept.length === 4"_s);
    requireBodyThatCompletes(program);
    checkRunThatCompleted(program);

    check(program, "kept[1].getX() === 7"_s);
    check(program, "kept[1] instanceof kept[2]"_s);
    check(program, "Object.getPrototypeOf(kept[1]) === kept[2].prototype"_s);
    check(program, "kept[2] !== loaded.Point && kept[2].prototype !== loaded.Point.prototype"_s);
    check(program, "kept[2].prototype.constructor === kept[2]"_s);
    check(program, "kept[2].made === 1"_s);
    check(program, "kept[2].extra === 'left by the failed run'"_s);
    check(program, "kept[2].prototype.getX.mark === 'left by the failed run'"_s);
    check(program, "new kept[2](9).getX() === 9"_s);
    check(program, "Object.getPrototypeOf(new kept[2](9)) === kept[2].prototype"_s);
    check(program, "kept[1][kept[3]] === 'tagged'"_s);
    check(program, "typeof loaded.inspect(kept[1]) !== 'undefined'"_s);
}

static void checkEscapedClosureKeepsVariablesOfItsRun(LoadedProgram& program)
{
    defineHostFunctions(program);
    requireBodyThatThrows(program);
    requireBodyThatCompletes(program);
    check(program, "loaded.next() === 2 && kept[0]() === 2 && loaded.next() === 3"_s);
}

static void checkEachRunCreatesItsSymbol(LoadedProgram& program)
{
    defineHostFunctions(program);
    requireBodyThatThrows(program);
    requireBodyThatCompletes(program);
    check(program, "kept[3] !== loaded.tag && kept[3].description === 'tag' && loaded.tag.description === 'tag'"_s);
}

static void checkEscapedClosureBetweenRuns(LoadedProgram& program)
{
    defineHostFunctions(program);
    requireBodyThatThrows(program);
    check(program, "kept[0]() === 2 && kept[0]() === 3"_s);
    check(program, "new kept[2](4).getX() === 4 && kept[2].made === 1"_s);
    requireBodyThatCompletes(program);
    checkRunThatCompleted(program);
}

static void checkHierarchy(LoadedProgram& program, ASCIILiteral base, ASCIILiteral derived, ASCIILiteral object)
{
    JSGlobalObject* realm = program.realm();
    VM& vm = program.vm();
    realm->putDirect(vm, Identifier::fromString(vm, "Base"_s), program.realms().evaluate(realm, base));
    realm->putDirect(vm, Identifier::fromString(vm, "Derived"_s), program.realms().evaluate(realm, derived));
    realm->putDirect(vm, Identifier::fromString(vm, "object"_s), program.realms().evaluate(realm, object));
    check(program, "object.readX() === 3 && object.getX() === 3"_s);
    check(program, "Base.kind() === 'a base' && Derived.kind() === 'a base'"_s);
    check(program, "Base.label === 'base' && Derived.label === 'base'"_s);
    check(program, "object instanceof Derived && object instanceof Base"_s);
    check(program, "new Derived(5).readX() === 5"_s);
    check(program, "Object.getPrototypeOf(object) === Derived.prototype"_s);
    check(program, "Object.getPrototypeOf(Derived.prototype) === Base.prototype"_s);
    check(program, "Object.getPrototypeOf(Derived) === Base"_s);
    check(program, "Object.getOwnPropertyNames(Base.prototype).length === 2 && Object.getOwnPropertyNames(Derived.prototype).length === 2"_s);
    check(program, "Derived.prototype.constructor === Derived && Base.prototype.constructor === Base"_s);
}

static void checkClassesOfEveryRunStayUsable(LoadedProgram& program)
{
    defineHostFunctions(program);
    requireBodyThatThrows(program);
    requireBodyThatThrows(program);
    requireBodyThatCompletes(program);
    check(program, "kept.length === 6"_s);
    checkHierarchy(program, "loaded.Base"_s, "loaded.Derived"_s, "loaded.object"_s);
    checkHierarchy(program, "kept[0]"_s, "kept[1]"_s, "kept[2]"_s);
    checkHierarchy(program, "kept[3]"_s, "kept[4]"_s, "kept[5]"_s);
    check(program, "kept[0] !== kept[3] && kept[3] !== loaded.Base && kept[1] !== kept[4] && kept[4] !== loaded.Derived"_s);
}

static void checkClassUsedBetweenResetAndDefinition(LoadedProgram& program)
{
    defineHostFunctions(program);
    requireBodyThatThrows(program);
    program.realms().evaluate(program.realm(), R"JS(
        var resultsAtStart = [];
        atStart = function () {
            resultsAtStart[resultsAtStart.length] = new kept[1](5).readX();
            resultsAtStart[resultsAtStart.length] = kept[2].readX();
            resultsAtStart[resultsAtStart.length] = kept[1].kind();
        };
    )JS"_s);
    requireBodyThatThrows(program);
    requireBodyThatCompletes(program);
    check(program, "resultsAtStart.length === 6"_s);
    check(program, "resultsAtStart[0] === 5 && resultsAtStart[1] === 3 && resultsAtStart[2] === 'a base'"_s);
    check(program, "resultsAtStart[3] === 5 && resultsAtStart[4] === 3 && resultsAtStart[5] === 'a base'"_s);
    checkHierarchy(program, "loaded.Base"_s, "loaded.Derived"_s, "loaded.object"_s);
    checkHierarchy(program, "kept[0]"_s, "kept[1]"_s, "kept[2]"_s);
    checkHierarchy(program, "kept[3]"_s, "kept[4]"_s, "kept[5]"_s);
}

static void checkEscapedClassUsedBeforeDeclarations(LoadedProgram& program)
{
    defineHostFunctions(program);
    requireBodyThatThrows(program);
    program.realms().evaluate(program.realm(), R"JS(
        var errorsAtStart = 0;
        var valueAtStart;
        atStart = function () {
            try { new kept[2](4); } catch (error) { errorsAtStart += error instanceof ReferenceError; }
            valueAtStart = kept[1].getX();
        };
    )JS"_s);
    requireBodyThatCompletes(program);
    check(program, "errorsAtStart === 0 && valueAtStart === 7"_s);
    checkRunThatCompleted(program);
    check(program, "kept[1].getX() === 7 && new kept[2](9).getX() === 9"_s);
}

template<const ASCIILiteral& programText, LoadedProgramTest body>
static constexpr void (*withProgram)() = runNativeTestWithLoadedProgram<commonJSRerunTests, programText, body>;

std::span<const NativeTest> commonJSRerunTests()
{
    static constexpr NativeTest tests[] = {
        { "commonjs-rerun/tables-list-cells-of-module-objects"_s, withProgram<programWithoutEscape, checkTablesListCellsOfModuleObjects> },
        { "commonjs-rerun/second-run-starts-afresh"_s, withProgram<programWithoutEscape, checkSecondRunStartsAfresh> },
        { "commonjs-rerun/third-run-starts-afresh"_s, withProgram<programWithoutEscape, checkThirdRunStartsAfresh> },
        { "commonjs-rerun/what-escaped-stays-usable"_s, withProgram<programWithEscape, checkWhatEscapedStaysUsable> },
        { "commonjs-rerun/escaped-closure-keeps-variables-of-its-run"_s, withProgram<programWithEscape, checkEscapedClosureKeepsVariablesOfItsRun> },
        { "commonjs-rerun/each-run-creates-its-symbol"_s, withProgram<programWithEscape, checkEachRunCreatesItsSymbol> },
        { "commonjs-rerun/escaped-closure-between-runs"_s, withProgram<programWithEscape, checkEscapedClosureBetweenRuns> },
        { "commonjs-rerun/classes-of-every-run-stay-usable"_s, withProgram<programWithClassHierarchy, checkClassesOfEveryRunStayUsable> },
        { "commonjs-rerun/class-used-between-reset-and-definition"_s, withProgram<programWithClassHierarchy, checkClassUsedBetweenResetAndDefinition> },
        { "commonjs-rerun/escaped-class-used-before-declarations"_s, withProgram<programWithEscape, checkEscapedClassUsedBeforeDeclarations> },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
