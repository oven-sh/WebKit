/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <wtf/Forward.h>

using WTF::UniquedStringImpl;

namespace JSC {

class JSGlobalObject;
class JSObject;
class JSValue;
class LazyClassStructure;
class PropertyName;
class Structure;
class VM;

namespace AOT {

enum class RefusedChange : uint8_t {
    Store,
    Definition,
    Deletion,
    Prototype,
};

} // namespace AOT

namespace AOT::RuntimeHooks {

void willInitializeRealm(JSGlobalObject*);
// The Structure that every host function of the realm is born with, made of the one that the engine would give them.
Structure* structureOfHostFunctions(JSGlobalObject*, Structure*);
// A native constructor is called like every built-in function. It has a Structure of its own, so it refuses by itself what that Structure refuses: nothing is done for it at start.
bool refusesNameOnNativeConstructor(JSGlobalObject*, PropertyName);
bool keepsPrototypeOfNativeConstructors(JSGlobalObject*);
String messageOfNameRefusedOnBuiltinFunction(PropertyName);
String messageOfPrototypeRefusedOnBuiltinFunction();
void didCreateRealm(JSGlobalObject*);
void didCreateClass(JSGlobalObject*, const LazyClassStructure&, Structure*, JSObject* prototype, JSObject* constructorOrNull);
void didCreateStaticProperty(JSObject* holder, UniquedStringImpl* key, JSValue);
void didChangePrototypeChain(VM&, JSObject* changed, UniquedStringImpl* nameOrNull);
void didFlattenObjectOnPrototypeChain(VM&, JSObject* flattened);
void didFlattenStructure(VM&, Structure*);
void didChangeMicrotaskQueueing(VM&);
void didChangeGlobalLexicalBindings(JSGlobalObject*);
void setLoopPollsEnabled(VM&, bool);
void didRefuseChange(JSGlobalObject*, JSObject*, UniquedStringImpl* keyOrNull, RefusedChange);
// Null for an object that start-up has not locked.
String messageOfRefusalByLockedObject(JSGlobalObject*, JSObject*, UniquedStringImpl* key, RefusedChange);

} } // namespace JSC::AOT::RuntimeHooks

#endif // ENABLE(AOT)
