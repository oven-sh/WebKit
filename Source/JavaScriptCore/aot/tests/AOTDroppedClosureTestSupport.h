/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTClosure.h"
#include "AOTClosureTable.h"
#include "AOTConvention.h"
#include "AOTFunctionNames.h"
#include "AOTFunctionTables.h"
#include "AOTLoadedProgramTestSupport.h"
#include "AOTObjectModel.h"
#include "AOTPropertyCaches.h"
#include "AOTPropertyCachesAnswers.h"
#include "AOTSlimClosure.h"
#include "AOTVector.h"
#include "JSCInlines.h"
#include "ObjectConstructor.h"
#include <wtf/text/MakeString.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC::AOT {

inline constexpr ASCIILiteral programWithClosuresToDrop =
    "module m0 path=\"/main.js\" displayPath=\"main.js\" body=f0\n"
    "entry module=m0\n"
    "variable g0 name=\"arrow\" module=m0 declared={type=anything} initialization=Undefined\n"
    "variable g1 name=\"plain\" module=m0 declared={type=anything} initialization=Undefined\n"
    "variable g2 name=\"asyncArrow\" module=m0 declared={type=anything} initialization=Undefined\n"
    "variable g3 name=\"holdsNine\" module=m0 declared={type=anything} initialization=Undefined\n"
    "function f0 name=\"main\" kind=Module results=[{type=anything}] {\n"
    "  b0():\n"
    "    v0 = Builtin builtin=GlobalThis\n"
    "    v1 = NewClosure function=f5\n"
    "    v2 = NewClosure function=f4\n"
    "    v3 = NewClosure function=f6\n"
    "    v4 = PropGet v0 key=\"register\" [isFreeName]\n"
    "    v5 = Call v4, -, v1, v2, v3\n"
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
    "    v8 = NewClosure v0, v0, v0, v0, v0, v0, v0, v0, v0 function=f7\n"
    "    PutModuleVariable v8 variable=g3\n"
    "    Return\n"
    "}\n"
    "function f6 name=\"readDisplayName\" module=m0 parameters=[{type=anything}] results=[{type=anything}] {\n"
    "  b0(v0: anything):\n"
    "    v1 = PropGet v0 key=\"displayName\" @7\n"
    "    Return v1\n"
    "}\n"
    "function f7 name=\"holdsNine\" kind=Arrow captures=[{type=anything}, {type=anything}, {type=anything}, {type=anything}, {type=anything}, {type=anything}, {type=anything}, {type=anything}, {type=anything}] results=[{type=anything}] {\n"
    "  b0(v0: anything, v1: anything, v2: anything, v3: anything, v4: anything, v5: anything, v6: anything, v7: anything, v8: anything):\n"
    "    Return v8\n"
    "}\n"_s;

inline constexpr ASCIILiteral namesOfFunctionsWithClosuresToDrop[] = { "arrow"_s, "plain"_s, "asyncArrow"_s, "holdsNine"_s, "callAll"_s, "create"_s, "readDisplayName"_s };
static_assert(ClosureRequest::maxSlimClosureCaptures < 9);

inline FunctionID findFunctionWithClosuresToDrop(LoadedProgram& program, ASCIILiteral name)
{
    const ProgramTables& tables = programTables();
    const uint64_t* closureWords = FunctionTables::of(program.vm()).words;
    for (uint32_t index = tables.closureFunctions.first; index < tables.closureFunctions.end; ++index) {
        FunctionID function(index);
        if (closureWords[closureNumberOf(function)] && !functionKindData(function).isInRuntimeModule && writtenFunctionName(function) == name)
            return function;
    }
    failNativeTest(__FILE__, __LINE__, name.characters());
}

inline JSObject* createClosureToDrop(LoadedProgram& program, FunctionID function)
{
    constexpr uint64_t captureWithoutReference = 0x3ff8000000000000ULL;
    CaptureCounts counts = captureCountsOf(function);
    Vector<uint64_t, 4> captures(counts.captures, [&](size_t index) {
        return index < counts.references ? static_cast<uint64_t>(JSValue::encode(jsNull())) : captureWithoutReference;
    });
    return program.state().objects->closures.createClosure(program.vm(), function, captures.span(), counts.references);
}

inline JSString* createTextToDrop(LoadedProgram& program, unsigned number)
{
    return jsNontrivialString(program.vm(), makeString("the display name of closure "_s, number));
}

inline JSObject* createSourceOfDisplayName(LoadedProgram& program, JSString* text)
{
    VM& vm = program.vm();
    JSObject* source = constructEmptyObject(program.realm());
    source->putDirect(vm, vm.propertyNames->displayName, text);
    return source;
}

inline void assignDisplayName(LoadedProgram& program, JSObject* closure, JSObject* source)
{
    VM& vm = program.vm();
    JSGlobalObject* realm = program.realm();
    MarkedArgumentBuffer arguments;
    arguments.append(closure);
    arguments.append(source);
    JSValue result = call(realm, realm->objectConstructor()->get(realm, Identifier::fromString(vm, "assign"_s)), arguments, "Object.assign is a function"_s);
    AOT_TEST_CHECK(result == JSValue(closure));
    AOT_TEST_CHECK(closure->getDirect(vm, vm.propertyNames->displayName) == source->getDirect(vm, vm.propertyNames->displayName));
}

inline CachedName displayNameInCaches(LoadedProgram& program)
{
    CachedName name = program.state().propertyCaches->nameOf(program.vm().propertyNames->displayName.impl());
    AOT_TEST_CHECK(name.identifierNumber);
    return name;
}

inline void fileReadOfDisplayName(LoadedProgram& program, JSObject* closure)
{
    VM& vm = program.vm();
    PropertyCaches& caches = *program.state().propertyCaches;
    Structure* structure = closure->structure();
    PropertyOffset offset = structure->get(vm, vm.propertyNames->displayName);
    AOT_TEST_CHECK(isValidOffset(offset));
    CachedName name = displayNameInCaches(program);
    caches.cache({ structure, name, SlotAccess::Read }, PropertyAnswer::own(PropertyAnswerKind::Value, SlotAccess::Read, offset));
    AOT_TEST_CHECK(caches.tables().find(structure->id(), name, SlotAccess::Read));
}

inline SiteCache siteOfTestsOfDroppedClosures(LoadedProgram& program)
{
    AOT_TEST_CHECK(!program.instance().siteCaches().empty());
    return program.state().propertyCaches->siteCaches().at(0);
}

inline void fillSiteWithReadOfDisplayName(LoadedProgram& program, JSObject* closure)
{
    SiteCache site = siteOfTestsOfDroppedClosures(program);
    program.state().propertyCaches->fillSiteCacheFromTables(site, closure, *displayNameInCaches(program).identifierNumber, SlotAccess::Read);
    AOT_TEST_CHECK(site->get().structureID == closure->structureID());
}

inline JSObject* createClosureWithSharedStructure(LoadedProgram& program, FunctionID function, JSObject* source)
{
    constexpr unsigned maxNumberOfClosuresMade = 4;
    for (unsigned i = 0; i < maxNumberOfClosuresMade; ++i) {
        JSObject* closure = createClosureToDrop(program, function);
        assignDisplayName(program, closure, source);
        if (!closure->structure()->isDictionary())
            return closure;
    }
    failNativeTest(__FILE__, __LINE__, "a closure gets a Structure that the next closure gets too");
}

inline JSObject* createClosureWithStructureOfItsOwn(LoadedProgram& program, FunctionID function, JSObject* source)
{
    JSObject* closure = createClosureToDrop(program, function);
    assignDisplayName(program, closure, source);
    if (!closure->structure()->isDictionary())
        closure->convertToDictionary(program.vm());
    if (FirstAdditions* additions = program.state().firstAdditions.get())
        additions->forget(closure);
    AOT_TEST_CHECK(closure->structure()->isDictionary() && !closure->structure()->isUncacheableDictionary());
    return closure;
}

} // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
