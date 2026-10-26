/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTClosureTable.h"
#include "AOTFunctionDescription.h"
#include "AOTFunctionNames.h"
#include "AOTFunctionTables.h"
#include "AOTLoadedProgramTestSupport.h"
#include "AOTObjectModel.h"
#include "AOTSlimClosure.h"
#include "AOTVector.h"
#include "AsyncFunctionPrototype.h"
#include "BuiltinNames.h"
#include "CustomGetterSetter.h"
#include "DeletePropertySlot.h"
#include "FunctionPrototype.h"
#include "GetterSetter.h"
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
    "function f2 name=\"plain\" length=2 captures=[{type=anything}, {type=number}] parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {\n"
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
    "    Return v5\n"
    "}\n"
    // Whoever got `create` may call it any number of times, so what the closures capture stays in their cells.
    "function f5 name=\"create\" parameters=[{type=anything}] results=[{type=anything}] {\n"
    "  b0(v0: anything):\n"
    "    v1 = ToNumber v0\n"
    "    v2 = NewClosure v0 function=f1\n"
    "    PutModuleVariable v2 variable=g0\n"
    "    v4 = NewClosure v0, v1 function=f2\n"
    "    PutModuleVariable v4 variable=g1\n"
    "    v6 = NewClosure v0 function=f3\n"
    "    PutModuleVariable v6 variable=g2\n"
    "    Return\n"
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
static constexpr unsigned readOnlyDontEnum = PropertyAttribute::ReadOnly | PropertyAttribute::DontEnum;

static FunctionID findSlimFunction(LoadedProgram& program, ASCIILiteral name)
{
    const ProgramTables& tables = programTables();
    const uint64_t* closureWords = FunctionTables::of(program.vm()).words;
    for (uint32_t index = tables.closureFunctions.first; index < tables.closureFunctions.end; ++index) {
        FunctionID function(index);
        if (closureWords[index] && functionKindData(function).isSlim && writtenFunctionName(function) == name)
            return function;
    }
    for (uint32_t index = tables.closureFunctions.first; index < tables.closureFunctions.end; ++index)
        dataLogLn("f", index, ": word ", closureWords[index], functionKindData(FunctionID(index)).isSlim ? " slim" : " full", " name \"", writtenFunctionName(FunctionID(index)), "\"");
    failNativeTest(__FILE__, __LINE__, name.characters());
}

static FunctionID findClosureFunction(LoadedProgram& program, ASCIILiteral name)
{
    const ProgramTables& tables = programTables();
    const uint64_t* closureWords = FunctionTables::of(program.vm()).words;
    for (uint32_t index = tables.closureFunctions.first; index < tables.closureFunctions.end; ++index) {
        FunctionID function(index);
        if (closureWords[index] && writtenFunctionName(function) == name)
            return function;
    }
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

static bool hasOwnDataProperty(LoadedProgram& program, JSObject* object, PropertyName name, JSValue value, unsigned attributes)
{
    PropertySlot slot(object, PropertySlot::InternalMethodType::GetOwnProperty);
    return getOwnPropertySlot(program, object, name, slot) && slot.getValue(program.realm(), name) == value && slot.attributes() == attributes;
}

static bool hasOwnStringProperty(LoadedProgram& program, JSObject* object, PropertyName name, ASCIILiteral text, unsigned attributes)
{
    PropertySlot slot(object, PropertySlot::InternalMethodType::GetOwnProperty);
    if (!getOwnPropertySlot(program, object, name, slot) || slot.attributes() != attributes)
        return false;
    JSValue value = slot.getValue(program.realm(), name);
    if (!value.isString())
        return false;
    String string = asString(value)->value(program.realm());
    return string == text;
}

static bool put(LoadedProgram& program, JSObject* object, PropertyName name, JSValue value, PutPropertySlot& slot)
{
    return object->methodTable()->put(object, program.realm(), name, value, slot);
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
        AOT_TEST_CHECK(word == FunctionTables::of(program.vm()).words[function.index()]);
        AOT_TEST_CHECK(ClosureWord::functionOf(word) == function.index());
        AOT_TEST_CHECK(ClosureWord::isCallable(word) && !ClosureWord::takesNewTarget(word));
        AOT_TEST_CHECK(closure->numberOfReferences() == counts.references);
        AOT_TEST_CHECK(equalSpans(closure->captures(), capturesOf(function, jsNull()).span()));
        AOT_TEST_CHECK(closure->canConstruct() == functionCase.canConstruct);
        AOT_TEST_CHECK(!closure->isBound());

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

static void testReadsOfNameAndLengthDoNotReify(LoadedProgram& program)
{
    VM& vm = program.vm();
    FunctionID function = findSlimFunction(program, "plain"_s);
    for (bool readsLengthFirst : { true, false }) {
        JSObject* closure = createSlimClosure(program, function);
        auto hasLength = [&] {
            return hasOwnDataProperty(program, closure, vm.propertyNames->length, jsNumber(2), readOnlyDontEnum);
        };
        auto hasName = [&] {
            return hasOwnStringProperty(program, closure, vm.propertyNames->name, "plain"_s, readOnlyDontEnum);
        };

        AOT_TEST_CHECK(readsLengthFirst ? hasLength() : hasName());
        AOT_TEST_CHECK(isSlim(closure));
        AOT_TEST_CHECK(readsLengthFirst ? hasName() : hasLength());
        AOT_TEST_CHECK(isSlim(closure));
        AOT_TEST_CHECK(hasLength() && hasLength() && hasName() && hasName() && hasName());
        AOT_TEST_CHECK(isSlim(closure));
    }
}

static void testReadOfPrototypeReifies(LoadedProgram& program)
{
    VM& vm = program.vm();
    FunctionID function = findSlimFunction(program, "plain"_s);
    JSObject* closure = createSlimClosure(program, function);

    JSValue prototype = closure->get(program.realm(), vm.propertyNames->prototype);
    AOT_TEST_CHECK(isReifiedClosureOf(closure, function));
    AOT_TEST_CHECK(prototype.isObject() && asObject(prototype)->getDirect(vm, vm.propertyNames->constructor) == JSValue(closure));
    AOT_TEST_CHECK(hasOwnDataProperty(program, closure, vm.propertyNames->prototype, prototype, PropertyAttribute::DontEnum | PropertyAttribute::DontDelete));
}

static void testPrototypeObjectIsMadeByItsFirstRead(LoadedProgram& program)
{
    VM& vm = program.vm();
    JSGlobalObject* realm = program.realm();
    FunctionID function = findSlimFunction(program, "plain"_s);
    Identifier label = Identifier::fromString(vm, "label"_s);
    constexpr unsigned attributes = PropertyAttribute::DontEnum | PropertyAttribute::DontDelete;

    JSObject* closure = createSlimClosure(program, function);
    AOT_TEST_CHECK(closure->putDirect(vm, label, jsNumber(7)) && isReifiedClosureOf(closure, function));
    AOT_TEST_CHECK(closure->getDirect(vm, vm.propertyNames->prototype).isCustomGetterSetter() && closure->hasOwnProperty(realm, vm.propertyNames->prototype));
    JSValue prototype = closure->get(realm, vm.propertyNames->prototype);
    AOT_TEST_CHECK(prototype.isObject() && asObject(prototype)->getDirect(vm, vm.propertyNames->constructor) == JSValue(closure));
    AOT_TEST_CHECK(closure->getDirect(vm, vm.propertyNames->prototype) == prototype && closure->get(realm, vm.propertyNames->prototype) == prototype);
    AOT_TEST_CHECK(hasOwnDataProperty(program, closure, vm.propertyNames->prototype, prototype, attributes));

    JSObject* readAtOnce = createSlimClosure(program, function);
    JSObject* readLater = createSlimClosure(program, function);
    DeletePropertySlot deleteSlot;
    AOT_TEST_CHECK(readLater->methodTable()->deleteProperty(readLater, realm, label, deleteSlot) && readLater->getDirect(vm, vm.propertyNames->prototype).isCustomGetterSetter());
    AOT_TEST_CHECK(readAtOnce->get(realm, vm.propertyNames->prototype).isObject() && readLater->get(realm, vm.propertyNames->prototype).isObject());
    AOT_TEST_CHECK(readAtOnce->structure() == readLater->structure());

    JSObject* storedTo = createSlimClosure(program, function);
    JSObject* storedPrototype = constructEmptyObject(realm);
    PutPropertySlot slot(storedTo, true);
    AOT_TEST_CHECK(storedTo->putDirect(vm, label, jsNumber(7)) && put(program, storedTo, vm.propertyNames->prototype, storedPrototype, slot));
    AOT_TEST_CHECK(hasOwnDataProperty(program, storedTo, vm.propertyNames->prototype, storedPrototype, attributes));

    JSObject* frozen = createSlimClosure(program, function);
    AOT_TEST_CHECK(frozen->putDirect(vm, label, jsNumber(7)));
    objectConstructorFreeze(realm, frozen);
    JSValue frozenPrototype = frozen->get(realm, vm.propertyNames->prototype);
    AOT_TEST_CHECK(frozenPrototype.isObject() && hasOwnDataProperty(program, frozen, vm.propertyNames->prototype, frozenPrototype, attributes | PropertyAttribute::ReadOnly));
}

static void testOwnPropertyNames(LoadedProgram& program)
{
    VM& vm = program.vm();
    for (const FunctionCase& functionCase : functionCases) {
        FunctionID function = findSlimFunction(program, functionCase.name);
        JSObject* closure = createSlimClosure(program, function);

        PropertyNameArrayBuilder names(vm, PropertyNameMode::StringsAndSymbols, PrivateSymbolMode::Exclude);
        closure->methodTable()->getOwnPropertyNames(closure, program.realm(), names, DontEnumPropertiesMode::Include);
        AOT_TEST_CHECK(isReifiedClosureOf(closure, function));
        AOT_TEST_CHECK(names.size() == (functionCase.canConstruct ? 3u : 2u));
        AOT_TEST_CHECK(names[0] == vm.propertyNames->length && names[1] == vm.propertyNames->name);
        AOT_TEST_CHECK(!functionCase.canConstruct || names[2] == vm.propertyNames->prototype);
        AOT_TEST_CHECK(hasOwnDataProperty(program, closure, vm.propertyNames->length, jsNumber(functionCase.length), readOnlyDontEnum));
        AOT_TEST_CHECK(hasOwnStringProperty(program, closure, vm.propertyNames->name, functionCase.name, readOnlyDontEnum));

        PropertyNameArrayBuilder enumerableNames(vm, PropertyNameMode::StringsAndSymbols, PrivateSymbolMode::Exclude);
        closure->methodTable()->getOwnPropertyNames(closure, program.realm(), enumerableNames, DontEnumPropertiesMode::Exclude);
        AOT_TEST_CHECK(!enumerableNames.size());
    }
}

static void testPut(LoadedProgram& program)
{
    VM& vm = program.vm();
    FunctionID function = findSlimFunction(program, "arrow"_s);
    Identifier label = Identifier::fromString(vm, "label"_s);

    JSObject* closure = createSlimClosure(program, function);
    PutPropertySlot slot(closure, true);
    AOT_TEST_CHECK(put(program, closure, label, jsNumber(7), slot));
    AOT_TEST_CHECK(!slot.isCacheablePut());
    AOT_TEST_CHECK(isReifiedClosureOf(closure, function));
    AOT_TEST_CHECK(closure->getDirect(vm, label) == jsNumber(7));

    JSObject* closureWithElement = createSlimClosure(program, function);
    AOT_TEST_CHECK(closureWithElement->methodTable()->putByIndex(closureWithElement, program.realm(), 3, jsNumber(9), true));
    AOT_TEST_CHECK(isReifiedClosureOf(closureWithElement, function));
    AOT_TEST_CHECK(closureWithElement->get(program.realm(), 3u) == jsNumber(9));

    AOT_TEST_CHECK(!FunctionTables::of(vm).hasDisplayNames.load());
    JSObject* closureWithDisplayName = createSlimClosure(program, function);
    PutPropertySlot displayNameSlot(closureWithDisplayName, true);
    AOT_TEST_CHECK(put(program, closureWithDisplayName, vm.propertyNames->displayName, jsNontrivialString(vm, "shown"_s), displayNameSlot));
    AOT_TEST_CHECK(!displayNameSlot.isCacheablePut());
    // The frames of this function do not record the function object that runs in them: the name stands for all of them.
    AOT_TEST_CHECK(captureCountsOf(function).captures && FunctionTables::of(vm).hasDisplayNames.load());
    AOT_TEST_CHECK(functionName(vm, FunctionDescription(function), NameForm::InFrames) == "shown"_s);

    // A store to `name` looks for it, also one that is refused.
    JSObject* closureWithReadOnlyName = createSlimClosure(program, function);
    PutPropertySlot sloppySlot(closureWithReadOnlyName, false);
    AOT_TEST_CHECK(!put(program, closureWithReadOnlyName, vm.propertyNames->name, jsNumber(7), sloppySlot));
    AOT_TEST_CHECK(isReifiedClosureOf(closureWithReadOnlyName, function));
    AOT_TEST_CHECK(hasOwnStringProperty(program, closureWithReadOnlyName, vm.propertyNames->name, "arrow"_s, readOnlyDontEnum));
}

static void storeDisplayName(LoadedProgram& program, JSObject* closure, ASCIILiteral displayName)
{
    VM& vm = program.vm();
    PutPropertySlot slot(closure, true);
    AOT_TEST_CHECK(put(program, closure, vm.propertyNames->displayName, jsNontrivialString(vm, displayName), slot));
}

static void testDisplayNameInFrames(LoadedProgram& program)
{
    VM& vm = program.vm();
    FunctionID function = findClosureFunction(program, "callAll"_s);
    AOT_TEST_CHECK(!captureCountsOf(function).captures);
    ClosureTable& closures = program.state().objects->closures;

    JSObject* closure = closures.createClosure(vm, function, { }, 0);
    AOT_TEST_CHECK(functionName(vm, FunctionDescription(function), NameForm::InFrames) == "callAll"_s);
    storeDisplayName(program, closure, "shown"_s);
    AOT_TEST_CHECK(functionName(vm, FunctionDescription(function), NameForm::InFrames) == "shown"_s);
    AOT_TEST_CHECK(functionName(vm, FunctionDescription(function), NameForm::AsWritten) == "callAll"_s);
    storeDisplayName(program, closure, "shown later"_s);
    AOT_TEST_CHECK(functionName(vm, FunctionDescription(function), NameForm::InFrames) == "shown later"_s);
    DeletePropertySlot deletion;
    AOT_TEST_CHECK(closure->methodTable()->deleteProperty(closure, program.realm(), vm.propertyNames->displayName, deletion));
    AOT_TEST_CHECK(functionName(vm, FunctionDescription(function), NameForm::InFrames) == "callAll"_s);
    storeDisplayName(program, closure, "shown again"_s);
    AOT_TEST_CHECK(functionName(vm, FunctionDescription(function), NameForm::InFrames) == "shown again"_s);

    JSObject* otherClosure = closures.createClosure(vm, function, { }, 0);
    storeDisplayName(program, otherClosure, "other"_s);
    AOT_TEST_CHECK(functionName(vm, FunctionDescription(function), NameForm::InFrames) == "callAll"_s);
    ensureStillAliveHere(closure);
}

static void testStoreToPrototypeIsNeverCached(LoadedProgram& program)
{
    VM& vm = program.vm();
    FunctionID function = findSlimFunction(program, "plain"_s);
    Identifier label = Identifier::fromString(vm, "label"_s);

    JSObject* storedTo = createSlimClosure(program, function);
    JSObject* storedPrototype = constructEmptyObject(program.realm());
    PutPropertySlot slot(storedTo, true);
    AOT_TEST_CHECK(put(program, storedTo, vm.propertyNames->prototype, storedPrototype, slot));
    AOT_TEST_CHECK(!slot.isCacheablePut());
    AOT_TEST_CHECK(isReifiedClosureOf(storedTo, function));
    AOT_TEST_CHECK(storedTo->getDirect(vm, vm.propertyNames->prototype) == JSValue(storedPrototype));

    JSObject* secondPrototype = constructEmptyObject(program.realm());
    PutPropertySlot secondSlot(storedTo, true);
    AOT_TEST_CHECK(put(program, storedTo, vm.propertyNames->prototype, secondPrototype, secondSlot));
    AOT_TEST_CHECK(!secondSlot.isCacheablePut());
    AOT_TEST_CHECK(storedTo->getDirect(vm, vm.propertyNames->prototype) == JSValue(secondPrototype));
    AOT_TEST_CHECK(storedTo->hasInstance(program.realm(), constructEmptyObject(program.realm(), secondPrototype)));
    AOT_TEST_CHECK(!storedTo->hasInstance(program.realm(), constructEmptyObject(program.realm(), storedPrototype)));

    PutPropertySlot otherSlot(storedTo, true);
    AOT_TEST_CHECK(put(program, storedTo, label, jsNumber(7), otherSlot));
    AOT_TEST_CHECK(otherSlot.isCacheablePut());
    AOT_TEST_CHECK(storedTo->getDirect(vm, label) == jsNumber(7));

    JSObject* redefined = createSlimClosure(program, function);
    JSObject* definedPrototype = constructEmptyObject(program.realm());
    PropertyDescriptor descriptor;
    descriptor.setValue(definedPrototype);
    AOT_TEST_CHECK(redefined->methodTable()->defineOwnProperty(redefined, program.realm(), vm.propertyNames->prototype, descriptor, true));
    AOT_TEST_CHECK(isReifiedClosureOf(redefined, function));
    AOT_TEST_CHECK(redefined->getDirect(vm, vm.propertyNames->prototype) == JSValue(definedPrototype));
}

static void testDefineOwnProperty(LoadedProgram& program)
{
    VM& vm = program.vm();
    JSGlobalObject* realm = program.realm();
    FunctionID function = findSlimFunction(program, "plain"_s);
    Identifier label = Identifier::fromString(vm, "label"_s);

    JSObject* closure = createSlimClosure(program, function);
    PropertyDescriptor labelDescriptor(jsNumber(7), static_cast<unsigned>(PropertyAttribute::DontEnum));
    AOT_TEST_CHECK(closure->methodTable()->defineOwnProperty(closure, realm, label, labelDescriptor, true));
    AOT_TEST_CHECK(isReifiedClosureOf(closure, function));
    AOT_TEST_CHECK(hasOwnDataProperty(program, closure, label, jsNumber(7), static_cast<unsigned>(PropertyAttribute::DontEnum)));

    AOT_TEST_CHECK(functionName(vm, FunctionDescription(function), NameForm::InFrames) == "plain"_s);
    JSObject* renamed = createSlimClosure(program, function);
    PropertyDescriptor nameDescriptor;
    nameDescriptor.setValue(jsNontrivialString(vm, "renamed"_s));
    AOT_TEST_CHECK(renamed->methodTable()->defineOwnProperty(renamed, realm, vm.propertyNames->name, nameDescriptor, true));
    AOT_TEST_CHECK(isReifiedClosureOf(renamed, function));
    AOT_TEST_CHECK(hasOwnStringProperty(program, renamed, vm.propertyNames->name, "renamed"_s, readOnlyDontEnum));
    AOT_TEST_CHECK(functionName(vm, FunctionDescription(function), NameForm::InFrames) == "renamed"_s);
}

static void testDeleteProperty(LoadedProgram& program)
{
    VM& vm = program.vm();
    JSGlobalObject* realm = program.realm();
    FunctionID function = findSlimFunction(program, "plain"_s);

    JSObject* closure = createSlimClosure(program, function);
    DeletePropertySlot lengthSlot;
    AOT_TEST_CHECK(closure->methodTable()->deleteProperty(closure, realm, vm.propertyNames->length, lengthSlot));
    AOT_TEST_CHECK(!lengthSlot.isCacheableDelete());
    AOT_TEST_CHECK(isReifiedClosureOf(closure, function));
    AOT_TEST_CHECK(!closure->hasOwnProperty(realm, vm.propertyNames->length));
    AOT_TEST_CHECK(closure->hasOwnProperty(realm, vm.propertyNames->name));

    JSObject* closureWithPrototype = createSlimClosure(program, function);
    DeletePropertySlot prototypeSlot;
    AOT_TEST_CHECK(!closureWithPrototype->methodTable()->deleteProperty(closureWithPrototype, realm, vm.propertyNames->prototype, prototypeSlot));
    AOT_TEST_CHECK(!prototypeSlot.isCacheableDelete());
    AOT_TEST_CHECK(closureWithPrototype->hasOwnProperty(realm, vm.propertyNames->prototype));

    JSObject* closureWithoutLabel = createSlimClosure(program, function);
    DeletePropertySlot absentSlot;
    AOT_TEST_CHECK(closureWithoutLabel->methodTable()->deleteProperty(closureWithoutLabel, realm, Identifier::fromString(vm, "label"_s), absentSlot));
    AOT_TEST_CHECK(!absentSlot.isCacheableDelete());
    AOT_TEST_CHECK(isReifiedClosureOf(closureWithoutLabel, function));
}

static void testPreventExtensions(LoadedProgram& program)
{
    VM& vm = program.vm();
    JSGlobalObject* realm = program.realm();
    FunctionID function = findSlimFunction(program, "arrow"_s);
    JSObject* closure = createSlimClosure(program, function);

    AOT_TEST_CHECK(closure->methodTable()->preventExtensions(closure, realm));
    AOT_TEST_CHECK(isReifiedClosureOf(closure, function));
    AOT_TEST_CHECK(!closure->methodTable()->isExtensible(closure, realm));

    Identifier label = Identifier::fromString(vm, "label"_s);
    PutPropertySlot sloppySlot(closure, false);
    AOT_TEST_CHECK(!put(program, closure, label, jsNumber(7), sloppySlot));
    AOT_TEST_CHECK(!closure->hasOwnProperty(realm, label));
}

static void testSetPrototype(LoadedProgram& program)
{
    JSGlobalObject* realm = program.realm();
    for (const FunctionCase& functionCase : functionCases) {
        FunctionID function = findSlimFunction(program, functionCase.name);
        JSObject* closure = createSlimClosure(program, function);

        AOT_TEST_CHECK(closure->methodTable()->setPrototype(closure, realm, closure->getPrototypeDirect(), true));
        AOT_TEST_CHECK(isSlim(closure));
        AOT_TEST_CHECK(closure->methodTable()->setPrototype(closure, realm, realm->objectPrototype(), true));
        AOT_TEST_CHECK(isReifiedClosureOf(closure, function));
        AOT_TEST_CHECK(closure->getPrototypeDirect() == JSValue(realm->objectPrototype()));
    }
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

static void testDirectDefinitionsReify(LoadedProgram& program)
{
    VM& vm = program.vm();
    JSGlobalObject* realm = program.realm();
    FunctionID function = findSlimFunction(program, "plain"_s);
    Identifier label = Identifier::fromString(vm, "label"_s);
    JSObject* captured = constructEmptyObject(realm);

    JSObject* closureWithValue = createSlimClosure(program, function, captured);
    AOT_TEST_CHECK(closureWithValue->putDirect(vm, label, jsNumber(7), static_cast<unsigned>(PropertyAttribute::DontEnum)));
    AOT_TEST_CHECK(isReifiedClosureOf(closureWithValue, function, captured));
    AOT_TEST_CHECK(hasOwnDataProperty(program, closureWithValue, label, jsNumber(7), static_cast<unsigned>(PropertyAttribute::DontEnum)));
    AOT_TEST_CHECK(hasOwnStringProperty(program, closureWithValue, vm.propertyNames->name, "plain"_s, readOnlyDontEnum));

    JSObject* closureWithCustomAccessor = createSlimClosure(program, function, captured);
    CustomGetterSetter* customAccessor = CustomGetterSetter::create(vm, getSeven, nullptr);
    AOT_TEST_CHECK(closureWithCustomAccessor->putDirectCustomAccessor(vm, label, customAccessor, PropertyAttribute::CustomAccessor | PropertyAttribute::ReadOnly));
    AOT_TEST_CHECK(isReifiedClosureOf(closureWithCustomAccessor, function, captured));
    AOT_TEST_CHECK(closureWithCustomAccessor->getDirect(vm, label) == JSValue(customAccessor));
    AOT_TEST_CHECK(closureWithCustomAccessor->get(realm, label) == jsNumber(7));

    JSObject* closureWithAccessor = createSlimClosure(program, function, captured);
    GetterSetter* accessor = GetterSetter::create(vm, realm, createSlimClosure(program, function), nullptr);
    AOT_TEST_CHECK(closureWithAccessor->putDirectAccessor(realm, label, accessor, static_cast<unsigned>(PropertyAttribute::Accessor)));
    AOT_TEST_CHECK(isReifiedClosureOf(closureWithAccessor, function, captured));
    AOT_TEST_CHECK(closureWithAccessor->getDirect(vm, label) == JSValue(accessor));
}

static void testIntegrityLevelsReify(LoadedProgram& program)
{
    VM& vm = program.vm();
    JSGlobalObject* realm = program.realm();
    JSObject* captured = constructEmptyObject(realm);
    for (const FunctionCase& functionCase : functionCases) {
        FunctionID function = findSlimFunction(program, functionCase.name);

        JSObject* sealedClosure = createSlimClosure(program, function, captured);
        sealedClosure->seal(vm);
        AOT_TEST_CHECK(isReifiedClosureOf(sealedClosure, function, captured));
        AOT_TEST_CHECK(sealedClosure->isSealed(vm));
        AOT_TEST_CHECK(!sealedClosure->methodTable()->isExtensible(sealedClosure, realm));
        AOT_TEST_CHECK(hasOwnStringProperty(program, sealedClosure, vm.propertyNames->name, functionCase.name, readOnlyDontEnum | PropertyAttribute::DontDelete));

        JSObject* frozenClosure = createSlimClosure(program, function, captured);
        frozenClosure->freeze(vm);
        AOT_TEST_CHECK(isReifiedClosureOf(frozenClosure, function, captured));
        AOT_TEST_CHECK(frozenClosure->isFrozen(vm));
        AOT_TEST_CHECK(hasOwnDataProperty(program, frozenClosure, vm.propertyNames->length, jsNumber(functionCase.length), readOnlyDontEnum | PropertyAttribute::DontDelete));
    }
}

static void testSetPrototypeDirect(LoadedProgram& program)
{
    VM& vm = program.vm();
    JSGlobalObject* realm = program.realm();
    JSObject* captured = constructEmptyObject(realm);
    for (const FunctionCase& functionCase : functionCases) {
        FunctionID function = findSlimFunction(program, functionCase.name);
        JSObject* closure = createSlimClosure(program, function, captured);

        closure->setPrototypeDirect(vm, closure->getPrototypeDirect());
        AOT_TEST_CHECK(isSlim(closure));
        closure->setPrototypeDirect(vm, realm->objectPrototype());
        AOT_TEST_CHECK(isReifiedClosureOf(closure, function, captured));
        AOT_TEST_CHECK(closure->getPrototypeDirect() == JSValue(realm->objectPrototype()));
    }
}

static void testPrivateNamesReify(LoadedProgram& program)
{
    VM& vm = program.vm();
    JSGlobalObject* realm = program.realm();
    FunctionID function = findSlimFunction(program, "arrow"_s);
    JSObject* captured = constructEmptyObject(realm);

    JSObject* brandedClosure = createSlimClosure(program, function, captured);
    Symbol* brand = Symbol::create(vm, PrivateName(PrivateName::PrivateSymbol, "brand"_s).uid());
    AOT_TEST_CHECK(!brandedClosure->hasPrivateBrand(realm, brand));
    AOT_TEST_CHECK(isSlim(brandedClosure));
    brandedClosure->setPrivateBrand(realm, brand);
    AOT_TEST_CHECK(isReifiedClosureOf(brandedClosure, function, captured));
    AOT_TEST_CHECK(brandedClosure->hasPrivateBrand(realm, brand));

    JSObject* closureWithField = createSlimClosure(program, function, captured);
    Identifier field = Identifier::fromUid(PrivateName(PrivateName::PrivateSymbol, "field"_s));
    AOT_TEST_CHECK(!closureWithField->hasPrivateField(realm, field));
    AOT_TEST_CHECK(isSlim(closureWithField));
    PutPropertySlot putSlot(closureWithField, true);
    closureWithField->definePrivateField(realm, field, jsNumber(7), putSlot);
    AOT_TEST_CHECK(!putSlot.isCacheablePut());
    AOT_TEST_CHECK(isReifiedClosureOf(closureWithField, function, captured));
    PropertySlot slot(closureWithField, PropertySlot::InternalMethodType::GetOwnProperty);
    AOT_TEST_CHECK(closureWithField->getPrivateField(realm, field, slot));
    AOT_TEST_CHECK(slot.getValue(realm, field) == jsNumber(7));
}

static void testPropertyRunIsRefused(LoadedProgram& program)
{
    VM& vm = program.vm();
    FunctionID function = findSlimFunction(program, "arrow"_s);
    Identifier first = Identifier::fromString(vm, "first"_s);
    Identifier second = Identifier::fromString(vm, "second"_s);
    UniquedStringImpl* names[] = { first.impl(), second.impl() };
    EncodedJSValue values[] = { JSValue::encode(jsNumber(1)), JSValue::encode(jsNumber(2)) };
    const PropertyRun* run = vm.propertyRuns.intern(names);

    JSObject* closure = createSlimClosure(program, function);
    AOT_TEST_CHECK(!closure->tryPutDirectPropertyRun(vm, *run, values));
    AOT_TEST_CHECK(isSlim(closure));
    AOT_TEST_CHECK(equalSpans(uncheckedDowncast<SlimClosure>(closure)->captures(), capturesOf(function, jsNull()).span()));

    uncheckedDowncast<SlimClosure>(closure)->reify(vm);
    AOT_TEST_CHECK(closure->tryPutDirectPropertyRun(vm, *run, values));
    AOT_TEST_CHECK(isReifiedClosureOf(closure, function));
    AOT_TEST_CHECK(closure->getDirect(vm, first) == jsNumber(1) && closure->getDirect(vm, second) == jsNumber(2));
}

static void testVisitDuringReify(LoadedProgram& program)
{
    VM& vm = program.vm();
    FunctionID function = findSlimFunction(program, "plain"_s);
    unsigned numberOfReferences = captureCountsOf(function).references;
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

static void testCapturesSurviveCollections(LoadedProgram& program)
{
    VM& vm = program.vm();
    for (const FunctionCase& functionCase : functionCases) {
        FunctionID function = findSlimFunction(program, functionCase.name);
        Weak<JSObject> captured(constructEmptyObject(program.realm()));
        Strong<JSObject> closure(vm, createSlimClosure(program, function, captured.get()));

        vm.heap.collectSync(CollectionScope::Full);
        AOT_TEST_CHECK(captured);
        AOT_TEST_CHECK(isSlim(closure.get()));
        AOT_TEST_CHECK(equalSpans(uncheckedDowncast<SlimClosure>(closure.get())->captures(), capturesOf(function, captured.get()).span()));

        uncheckedDowncast<SlimClosure>(closure.get())->reify(vm);
        vm.heap.collectSync(CollectionScope::Full);
        AOT_TEST_CHECK(captured);
        AOT_TEST_CHECK(isReifiedClosureOf(closure.get(), function, captured.get()));
        AOT_TEST_CHECK(hasOwnStringProperty(program, closure.get(), vm.propertyNames->name, functionCase.name, readOnlyDontEnum));
    }
}

template<LoadedProgramTest body>
static constexpr void (*withProgram)() = runNativeTestWithLoadedProgram<slimClosureTests, programText, body>;

std::span<const NativeTest> slimClosureTests()
{
    static constexpr NativeTest tests[] = {
        { "slim-closure/created-cells-are-slim"_s, withProgram<testCreatedCellsAreSlim> },
        { "slim-closure/hooks-without-effect-keep-cell-slim"_s, withProgram<testHooksWithoutEffectKeepCellSlim> },
        { "slim-closure/reads-of-name-and-length-do-not-reify"_s, withProgram<testReadsOfNameAndLengthDoNotReify> },
        { "slim-closure/read-of-prototype-reifies"_s, withProgram<testReadOfPrototypeReifies> },
        { "slim-closure/prototype-object-is-made-by-its-first-read"_s, withProgram<testPrototypeObjectIsMadeByItsFirstRead> },
        { "slim-closure/own-property-names"_s, withProgram<testOwnPropertyNames> },
        { "slim-closure/put"_s, withProgram<testPut> },
        { "slim-closure/display-name-in-frames"_s, withProgram<testDisplayNameInFrames> },
        { "slim-closure/store-to-prototype-is-never-cached"_s, withProgram<testStoreToPrototypeIsNeverCached> },
        { "slim-closure/define-own-property"_s, withProgram<testDefineOwnProperty> },
        { "slim-closure/delete-property"_s, withProgram<testDeleteProperty> },
        { "slim-closure/prevent-extensions"_s, withProgram<testPreventExtensions> },
        { "slim-closure/set-prototype"_s, withProgram<testSetPrototype> },
        { "slim-closure/has-instance"_s, withProgram<testHasInstance> },
        { "slim-closure/direct-definitions-reify"_s, withProgram<testDirectDefinitionsReify> },
        { "slim-closure/integrity-levels-reify"_s, withProgram<testIntegrityLevelsReify> },
        { "slim-closure/direct-prototype-change-reifies"_s, withProgram<testSetPrototypeDirect> },
        { "slim-closure/private-names-reify"_s, withProgram<testPrivateNamesReify> },
        { "slim-closure/property-run-is-refused"_s, withProgram<testPropertyRunIsRefused> },
        { "slim-closure/visit-during-reify"_s, withProgram<testVisitDuringReify> },
        { "slim-closure/captures-survive-collections"_s, withProgram<testCapturesSurviveCollections> },
    };
    return tests;
}

} // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
