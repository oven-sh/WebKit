/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTRealmStartup.h"

namespace JSC {

class PropertyName;

namespace AOT {

enum class RefusedChange : uint8_t { Store, Definition, Deletion, Prototype };

void didChangeChainObject(VM&, JSObject* changed, UniquedStringImpl* uidOrNull);

} // namespace AOT

// What the engine outside aot/ calls.
namespace AOT::RuntimeHooks {

using AOT::didCreateClass;
using AOT::didCreateStaticProperty;
using AOT::didRefuseChange;
// Null for an object that start-up has not locked.
using AOT::messageOfRefusalByLockedObject;

void willInitializeRealm(JSGlobalObject*);
// The Structure that every host function of the realm is born with, made of the one that the engine would give them.
Structure* structureOfHostFunctions(JSGlobalObject*, Structure*);
// A native constructor is called like every built-in function. It has a Structure of its own, so it refuses by itself what that Structure refuses: nothing is done for it at start.
bool refusesNameOnNativeConstructor(JSGlobalObject*, PropertyName);
inline bool keepsPrototypeOfNativeConstructors(JSGlobalObject* realm) { return locksBuiltins(realm); }
String messageOfNameRefusedOnBuiltinFunction(PropertyName);
String messageOfPrototypeRefusedOnBuiltinFunction();
void didCreateRealm(JSGlobalObject*);
inline void didChangePrototypeChain(VM& vm, JSObject* changed, UniquedStringImpl* nameOrNull) { didChangeChainObject(vm, changed, nameOrNull); }
// A cacheable dictionary becomes an ordinary object: every name finds what it found, at the offset where it lay, and the prototype stays.
void didFlattenObjectOnPrototypeChain(VM&, JSObject* flattened);
void didFlattenStructure(VM&, Structure*);
void didChangeMicrotaskQueueing(VM&);
void didChangeGlobalLexicalBindings(JSGlobalObject*);
void setLoopPollsEnabled(VM&, bool);

} } // namespace JSC::AOT::RuntimeHooks

#endif // ENABLE(AOT)
