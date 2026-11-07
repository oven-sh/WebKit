/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTClosureTable.h"
#include "AOTConvention.h"
#include "AOTFunctionNames.h"
#include "AOTFunctionTables.h"
#include "AOTLoadedProgramTestSupport.h"
#include "AOTObjectModel.h"
#include "AOTOperationCallScope.h"
#include "AOTSchemaRuntimeOperations.h"
#include "AOTSlimClosure.h"
#include "AOTVector.h"
#include "AsyncFunctionPrototype.h"
#include "BuiltinNames.h"
#include "CustomGetterSetter.h"
#include "DeletePropertySlot.h"
#include "FunctionPrototype.h"
#include "GetterSetter.h"
#include "HeapCellInlines.h"
#include "JSCInlines.h"
#include "ObjectConstructor.h"
#include "ObjectPrototype.h"
#include "PropertyDescriptor.h"
#include "PrivateName.h"
#include "PropertyNameArray.h"
#include "PropertyRun.h"
#include "PutPropertySlot.h"
#include "StrongInlines.h"
#include "Symbol.h"
#include "WeakInlines.h"
#include <algorithm>
#include <span>
#include <wtf/StdLibExtras.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC::AOT {

std::span<const NativeTest> slimClosureTests();

static constexpr ASCIILiteral programText =
    "module m0 path=\"/main.js\" displayPath=\"main.js\" body=f0\n"
    "entry module=m0\n"
    "variable g0 name=\"arrow\" module=m0 declared={type=anything} initialization=Undefined\n"
    "variable g1 name=\"plain\" module=m0 declared={type=anything} initialization=Undefined\n"
    "variable g2 name=\"asyncArrow\" module=m0 declared={type=anything} initialization=Undefined\n"
    "variable g3 name=\"holdsSeven\" module=m0 declared={type=anything} initialization=Undefined\n"
    "function f0 name=\"main\" kind=Module results=[{type=anything}] {\n"
    "  b0():\n"
    "    v0 = Builtin builtin=GlobalThis\n"
    "    v1 = NewClosure function=f5\n"
    "    v2 = NewClosure function=f4\n"
    "    v3 = PropGet v0 key=\"register\" [isFreeName]\n"
    "    v4 = Call v3, -, v1, v2\n"
    "    Return\n"
    "}\n"
    "function f1 name=\"arrow\" kind=Arrow captures=[{type=anything}] results=[{type=anything}] {\n"
    "  b0(v0: anything):\n"
    "    Return v0\n"
    "}\n"
    "function f2 name=\"plain\" length=2 captures=[{type=anything}, {type=number flags=[IsFromCompiler]}] parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {\n"
    "  b0(v0: anything, v1: number, v2: anything, v3: anything):\n"
    "    v4 = Call v0, -, v1, v2, v3\n"
    "    Return v4\n"
    "}\n"
    "function f3 name=\"asyncArrow\" kind=Arrow flags=[IsAsync] captures=[{type=anything}] results=[{type=anything}] {\n"
    "  b0(v0: anything):\n"
    "    v1 = Await v0\n"
    "    Return v1\n"
    "}\n"
    "function f4 name=\"callAll\" results=[{type=anything}] {\n"
    "  b0():\n"
    "    v0 = GetModuleVariable variable=g0\n"
    "    v1 = Call v0 [isNeverInlined]\n"
    "    v2 = GetModuleVariable variable=g1\n"
    "    v3 = Call v2, -, v1, v1 [isNeverInlined]\n"
    "    v4 = GetModuleVariable variable=g2\n"
    "    v5 = Call v4 [isNeverInlined]\n"
    "    v6 = GetModuleVariable variable=g3\n"
    "    v7 = Call v6 [isNeverInlined]\n"
    "    Return v5\n"
    "}\n"
    "function f5 name=\"create\" parameters=[{type=anything}] results=[{type=anything}] {\n"
    "  b0(v0: anything):\n"
    "    v1 = ToNumber v0\n"
    "    v2 = NewClosure v0 function=f1\n"
    "    PutModuleVariable v2 variable=g0\n"
    "    v4 = NewClosure v0, v1 function=f2\n"
    "    PutModuleVariable v4 variable=g1\n"
    "    v6 = NewClosure v0 function=f3\n"
    "    PutModuleVariable v6 variable=g2\n"
    "    v8 = NewClosure v0, v0, v0, v0, v0, v0, v0 function=f6\n"
    "    PutModuleVariable v8 variable=g3\n"
    "    Return\n"
    "}\n"
    "function f6 name=\"holdsSeven\" kind=Arrow captures=[{type=anything}, {type=anything}, {type=anything}, {type=anything}, {type=anything}, {type=anything}, {type=anything}] results=[{type=anything}] {\n"
    "  b0(v0: anything, v1: anything, v2: anything, v3: anything, v4: anything, v5: anything, v6: anything):\n"
    "    v7 = Call v0, -, v1, v2, v3, v4, v5, v6\n"
    "    Return v7\n"
    "}\n"_s;

namespace {

struct FunctionCase {
    ASCIILiteral name;
    bool canConstruct;
    bool isAsync;
    unsigned length;
};

struct RecordingVisitor {
    void appendUnbarriered(JSValue value) { values.append(value); }

    Vector<JSValue, 4> values;
};

} // anonymous namespace

static JSC_DECLARE_CUSTOM_GETTER(getSeven);

JSC_DEFINE_CUSTOM_GETTER(getSeven, (JSGlobalObject*, EncodedJSValue, PropertyName))
{
    return JSValue::encode(jsNumber(7));
}

static constexpr FunctionCase functionCases[] = {
    { "arrow"_s, false, false, 0 },
    { "plain"_s, true, false, 2 },
    { "asyncArrow"_s, false, true, 0 },
};

static constexpr uint64_t captureWithoutReference = 0x3ff8000000000000ULL;

static FunctionID findSlimFunction(LoadedProgram& program, ASCIILiteral name)
{
    const ProgramTables& tables = programTables();
    const uint64_t* closureWords = FunctionTables::of(program.vm()).words;
    for (uint32_t index = tables.closureFunctions.first; index < tables.closureFunctions.end; ++index) {
        FunctionID function(index);
        if (closureWords[closureNumberOf(function)] && functionKindData(function).isSlim && writtenFunctionName(function) == name)
            return function;
    }
    for (uint32_t index = tables.closureFunctions.first; index < tables.closureFunctions.end; ++index)
        dataLogLn("f", index, ": word ", closureWords[tables.closureNumberOf(index)], functionKindData(FunctionID(index)).isSlim ? " slim" : " full", " name \"", writtenFunctionName(FunctionID(index)), "\"");
    failNativeTest(__FILE__, __LINE__, name.characters());
}

static Vector<uint64_t, 4> capturesOf(FunctionID function, JSValue reference)
{
    CaptureCounts counts = captureCountsOf(function);
    return Vector<uint64_t, 4>(counts.captures, [&](size_t index) {
        return index < counts.references ? static_cast<uint64_t>(JSValue::encode(reference)) : captureWithoutReference;
    });
}

static SlimClosure* createSlimClosure(LoadedProgram& program, FunctionID function, JSValue reference = jsNull())
{
    VM& vm = program.vm();
    Vector<uint64_t, 4> captures = capturesOf(function, reference);
    Structure* structure = program.state().objects->closures.slimClosureStructure(vm, function);
    return SlimClosure::create(vm, structure, function, captures.span(), captureCountsOf(function).references);
}

static bool isSlim(JSObject* closure)
{
    return closure->type() == SlimClosureType && closure->classInfo() == SlimClosure::info();
}

static bool isReifiedClosureOf(JSObject* closure, FunctionID function, JSValue reference = jsNull())
{
    if (closure->type() != SlimClosureWithPropertiesType || closure->classInfo() != ReifiedSlimClosure::info())
        return false;
    auto* reified = uncheckedDowncast<ReifiedSlimClosure>(closure);
    Vector<uint64_t, 4> captures = capturesOf(function, reference);
    return reified->function() == function && equalSpans(reified->captures(), captures.span());
}

static bool getOwnPropertySlot(LoadedProgram& program, JSObject* object, PropertyName name, PropertySlot& slot)
{
    return object->methodTable()->getOwnPropertySlot(object, program.realm(), name, slot);
}

static void testCreatedCellsAreSlim(LoadedProgram& program)
{
    JSGlobalObject* realm = program.realm();
    for (const FunctionCase& functionCase : functionCases) {
        FunctionID function = findSlimFunction(program, functionCase.name);
        CaptureCounts counts = captureCountsOf(function);
        AOT_TEST_CHECK(counts.references);

        SlimClosure* closure = createSlimClosure(program, function);
        AOT_TEST_CHECK(isSlim(closure));
        AOT_TEST_CHECK(closure->function() == function);
        uint64_t word = *reinterpret_cast<const uint64_t*>(reinterpret_cast<const uint8_t*>(closure) + SlimClosure::offsetOfWord());
        AOT_TEST_CHECK(word == FunctionTables::of(program.vm()).words[closureNumberOf(function)]);
        AOT_TEST_CHECK(functionOfClosureWord(word) == function);
        AOT_TEST_CHECK(ClosureWord::isCallable(word) && !ClosureWord::takesNewTarget(word));
        AOT_TEST_CHECK(closure->numberOfReferences() == counts.references);
        AOT_TEST_CHECK(equalSpans(closure->captures(), capturesOf(function, jsNull()).span()));
        AOT_TEST_CHECK(closure->canConstruct() == functionCase.canConstruct);

        JSObject* prototype = functionCase.isAsync ? static_cast<JSObject*>(realm->asyncFunctionPrototype()) : realm->functionPrototype();
        AOT_TEST_CHECK(closure->getPrototypeDirect() == JSValue(prototype));

        CallData callData = closure->methodTable()->getCallData(closure);
        AOT_TEST_CHECK(callData.type == CallData::Type::Compiled && callData.compiled.realm == realm);
        CallData constructData = closure->methodTable()->getConstructData(closure);
        AOT_TEST_CHECK(constructData.type == (functionCase.canConstruct ? CallData::Type::Compiled : CallData::Type::None));
        AOT_TEST_CHECK(isSlim(closure));
    }
}

static void testHooksWithoutEffectKeepCellSlim(LoadedProgram& program)
{
    VM& vm = program.vm();
    JSGlobalObject* realm = program.realm();
    JSObject* closure = createSlimClosure(program, findSlimFunction(program, "arrow"_s));
    const MethodTable* methodTable = closure->methodTable();
    const Identifier& call = vm.propertyNames->builtinNames().callPublicName();

    Identifier absentNames[] = { Identifier::fromString(vm, "label"_s), vm.propertyNames->prototype, vm.propertyNames->hasInstanceSymbol, call };
    for (const Identifier& name : absentNames) {
        PropertySlot slot(closure, PropertySlot::InternalMethodType::GetOwnProperty);
        AOT_TEST_CHECK(!getOwnPropertySlot(program, closure, name, slot));
    }
    PropertySlot indexSlot(closure, PropertySlot::InternalMethodType::GetOwnProperty);
    AOT_TEST_CHECK(!methodTable->getOwnPropertySlotByIndex(closure, realm, 0, indexSlot));

    JSValue inheritedCall = closure->get(realm, call);
    AOT_TEST_CHECK(inheritedCall.isCallable() && inheritedCall == realm->functionPrototype()->get(realm, call));

    AOT_TEST_CHECK(methodTable->deletePropertyByIndex(closure, realm, 0));
    AOT_TEST_CHECK(methodTable->isExtensible(closure, realm));
    AOT_TEST_CHECK(methodTable->setPrototype(closure, realm, realm->functionPrototype(), true));
    AOT_TEST_CHECK(!closure->hasInstance(realm, jsNumber(1)));
    AOT_TEST_CHECK(isSlim(closure));
}

static void testHasInstance(LoadedProgram& program)
{
    VM& vm = program.vm();
    JSGlobalObject* realm = program.realm();
    FunctionID function = findSlimFunction(program, "plain"_s);
    JSObject* closure = createSlimClosure(program, function);

    AOT_TEST_CHECK(!closure->hasInstance(realm, constructEmptyObject(realm)));
    AOT_TEST_CHECK(isReifiedClosureOf(closure, function));

    JSObject* prototype = asObject(closure->getDirect(vm, vm.propertyNames->prototype));
    JSObject* instance = constructEmptyObject(realm, prototype);
    AOT_TEST_CHECK(closure->hasInstance(realm, instance));
    AOT_TEST_CHECK(closure->hasInstance(realm, constructEmptyObject(realm, instance)));
    AOT_TEST_CHECK(!createSlimClosure(program, function)->hasInstance(realm, instance));
}

static void testVisitDuringReify(LoadedProgram& program)
{
    VM& vm = program.vm();
    FunctionID function = findSlimFunction(program, "plain"_s);
    unsigned numberOfReferences = captureCountsOf(function).references;
    AOT_TEST_CHECK(numberOfReferences < captureCountsOf(function).captures);
    JSObject* captured = constructEmptyObject(program.realm());
    SlimClosure* closure = createSlimClosure(program, function, captured);
    auto visitedOnlyCaptured = [&](const RecordingVisitor& visitor) {
        return visitor.values.size() == numberOfReferences && std::ranges::all_of(visitor.values, [&](JSValue value) {
            return value == JSValue(captured);
        });
    };

    RecordingVisitor beforeReify;
    AOT_TEST_CHECK(SlimClosure::tryVisitFields(closure, beforeReify));
    AOT_TEST_CHECK(visitedOnlyCaptured(beforeReify));

    StructureID structureID = closure->structureID();
    closure->setStructureIDDirectly(structureID.nuke());
    RecordingVisitor duringReify;
    AOT_TEST_CHECK(!SlimClosure::tryVisitFields(closure, duringReify));
    AOT_TEST_CHECK(duringReify.values.isEmpty());
    closure->setStructureIDDirectly(structureID);

    ReifiedSlimClosure* reified = closure->reify(vm);
    RecordingVisitor afterReify;
    AOT_TEST_CHECK(!SlimClosure::tryVisitFields(closure, afterReify));
    AOT_TEST_CHECK(afterReify.values.isEmpty());

    RecordingVisitor reifiedVisitor;
    ReifiedSlimClosure::visitFields(reified, reifiedVisitor);
    AOT_TEST_CHECK(visitedOnlyCaptured(reifiedVisitor));

    Structure* structure = program.state().objects->closures.slimClosureStructure(vm, function);
    SlimClosure* unfilledClosure = SlimClosure::createForCompiledCode(vm, structure, SlimClosure::allocationSize(captureCountsOf(function).captures));
    RecordingVisitor unfilledVisitor;
    AOT_TEST_CHECK(SlimClosure::tryVisitFields(unfilledClosure, unfilledVisitor));
    AOT_TEST_CHECK(unfilledVisitor.values.isEmpty());
}

static void testWordsBehindRegistersAreEmptyUntilCodeFillsThem(LoadedProgram& program)
{
    FunctionID function = findSlimFunction(program, "holdsSeven"_s);
    CaptureCounts counts = captureCountsOf(function);
    AOT_TEST_CHECK(counts.references > ClosureRequest::maxSlimClosureCapturesInRegisters && counts.captures <= ClosureRequest::maxSlimClosureCaptures);
    JSObject* captured = constructEmptyObject(program.realm());
    uint64_t inRegister = static_cast<uint64_t>(JSValue::encode(captured));
    uint64_t request = closureNumberOf(function) | static_cast<uint64_t>(functionKindData(function).slimClosureKind()) << ClosureRequest::executableShift | 1ULL << ClosureRequest::isSlimBit | static_cast<uint64_t>(counts.captures) << ClosureRequest::countShift;
    JSCell* closure = callOperationFromTest(program.vm(), operationAOTNewClosureSlow, &program.instance(), request, inRegister, inRegister, inRegister, inRegister);
    AOT_TEST_CHECK(isSlim(asObject(closure)) && closure->cellSize() >= SlimClosure::allocationSize(counts.captures));

    RecordingVisitor visitor;
    AOT_TEST_CHECK(SlimClosure::tryVisitFields(uncheckedDowncast<SlimClosure>(closure), visitor));
    AOT_TEST_CHECK(visitor.values.size() == counts.references);
    for (unsigned slot = 0; slot < counts.references; ++slot)
        AOT_TEST_CHECK(visitor.values[slot] == (slot < ClosureRequest::maxSlimClosureCapturesInRegisters ? JSValue(captured) : JSValue()));
}

template<LoadedProgramTest body>
static constexpr void (*withProgram)() = runNativeTestWithLoadedProgram<slimClosureTests, programText, body>;

static constexpr ASCIILiteral noRawCaptures = "plan/AOTFunctionKinds.cpp: raw captures: closureCaptureRepresentationOf() gives every capture a boxed word, so no closure holds a word that the collector must not visit"_s;

std::span<const NativeTest> slimClosureTests()
{
    static constexpr NativeTest tests[] = {
        { "slim-closure/created-cells-are-slim"_s, withProgram<testCreatedCellsAreSlim> },
        { "slim-closure/hooks-without-effect-keep-cell-slim"_s, withProgram<testHooksWithoutEffectKeepCellSlim> },
        { "slim-closure/has-instance"_s, withProgram<testHasInstance> },
        { "slim-closure/visit-during-reify"_s, withProgram<testVisitDuringReify>, noRawCaptures },
        { "slim-closure/words-behind-the-registers-are-empty-until-code-fills-them"_s, withProgram<testWordsBehindRegistersAreEmptyUntilCodeFillsThem> },
    };
    return tests;
}

} // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
