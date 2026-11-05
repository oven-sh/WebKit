/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTRealmStartup.h"

namespace JSC {

class InlineWatchpointSet;
class JSArray;
class ObjectPropertyCondition;
class PropertyName;

namespace AOT {

enum class RejectedWrite : uint8_t { Store, Definition, Deletion, Prototype };

void didChangeChainObject(VM&, JSObject* changed, UniquedStringImpl* uidOrNull);

} // namespace AOT

// What the engine outside aot/ calls.
namespace AOT::RuntimeHooks {

using AOT::didCreateClass;
using AOT::didCreateStaticProperty;
using AOT::didRejectWrite;
// Null for an object that start-up has not locked.
using AOT::lockedObjectErrorMessage;
using AOT::reflectThrowsOnLockedProperty;
using AOT::takeStaticPropertyMadeAhead;

void willInitializeRealm(JSGlobalObject*);
// The Structure that every host function of the realm is born with, made of the one that the engine would give them.
Structure* structureOfHostFunctions(JSGlobalObject*, Structure*);
// A native constructor is called like every built-in function. It has a Structure of its own, so it refuses by itself what that Structure refuses: nothing is done for it at start.
bool refusesNameOnNativeConstructor(JSGlobalObject*, PropertyName);
inline bool keepsPrototypeOfNativeConstructors(JSGlobalObject* realm) { return locksBuiltins(realm); }
String messageOfNameRefusedOnBuiltinFunction(PropertyName);
String messageOfPrototypeRefusedOnBuiltinFunction();
void didCreateRealm(JSGlobalObject*);
// The built-ins of a realm that runs a program get other Structures when they are locked. What watches them is installed behind that, once.
enum class DeferredWatchpointKind : bool { Structure, PropertyChange };
void willWatchBuiltins(JSGlobalObject*);
void didWatchBuiltins(JSGlobalObject*);
bool defersWatchpoints(JSGlobalObject*);
bool deferWatchpoint(JSGlobalObject*, const ObjectPropertyCondition&, InlineWatchpointSet&, DeferredWatchpointKind);
void installDeferredWatchpoints(JSGlobalObject*);
// The stubs of an image ask words of the instance that are written at the start alone: they would go on making arrays with plain storage.
[[noreturn]] void stopBecauseRealmHasABadTime();
inline void didChangePrototypeChain(VM& vm, JSObject* changed, UniquedStringImpl* nameOrNull) { didChangeChainObject(vm, changed, nameOrNull); }
// No object with this Structure has got this name before. An object that is filled once keeps its names in a table of its own, without a Structure for each. The second object that gets the same name behind the same names gets Structures, which the third shares.
bool becomesDictionaryByAddition(VM&, JSObject*, Structure*, UniquedStringImpl*);
void didMakeStructureForAddition(VM&, Structure* made);
// The code that gave those objects their names has returned to the event loop: they are made flat, and are objects like any other from here on.
void finishFillingObjects(VM&);
void runFilesThatWaitInLoader(VM&);
// From now on a store that replaces a value of some Structure has somebody to tell.
void didStartWatchingReplacements(VM&);
void didChangeMicrotaskQueueing(VM&);
void didChangeGlobalLexicalBindings(JSGlobalObject*);
void setLoopPollsEnabled(VM&, bool);
// A store for another receiver ends in a question to the receiver and a definition on it. A proxy asks its handler for two traps and, where it has neither, hands both on to its target: that target.
// Null for every other value, and where finding out would run code of the program.
JSObject* targetThatProxyDefinesOn(VM&, JSValue receiver);

// For tryCreateArrayFromIterableFast(), which lists the keys and the values of a Map in one piece and leaves its pairs to the steps. Null: the caller goes on as ever.
JSArray* tryCreateArrayOfPairs(JSGlobalObject*, JSCell* items);

// A build runs code in a realm of its own, on one of its threads (facts/AOTFactsRegExpTexts.cpp). The system may refuse a build a thread, so the engine starts none for what runs in the scope:
// bytecode stays in the interpreter, no thread makes blocks of the heap ready, and a collection marks on the thread that runs it.
class NoHelperThreadsScope {
    WTF_MAKE_NONCOPYABLE(NoHelperThreadsScope);
public:
    NoHelperThreadsScope();
    ~NoHelperThreadsScope();
};
bool isInNoHelperThreadsScope();

} } // namespace JSC::AOT::RuntimeHooks

#endif // ENABLE(AOT)
