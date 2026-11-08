/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTRealmStartup.h"
#include <atomic>

namespace JSC {

class InlineWatchpointSet;
class JSArray;
class JSCell;
class JSFunction;
class ObjectPropertyCondition;
class PropertyName;

namespace AOT {

enum class RejectedWrite : uint8_t { Store, Definition, Deletion, Prototype };

void didChangeChainObject(VM&, JSObject* changed, UniquedStringImpl* uidOrNull);

} // namespace AOT

namespace AOT::RuntimeHooks {

using AOT::didCreateClass;
using AOT::didCreateStaticProperty;
using AOT::didRejectWrite;
using AOT::lockedObjectErrorMessage;
using AOT::reflectThrowsOnLockedProperty;
using AOT::takeStaticPropertyMadeAhead;

void willInitializeRealm(JSGlobalObject*);
Structure* structureOfHostFunctions(JSGlobalObject*, Structure*);
bool refusesNameOnNativeConstructor(JSGlobalObject*, PropertyName);
inline bool keepsPrototypeOfNativeConstructors(JSGlobalObject* realm) { return locksBuiltins(realm); }
String messageOfNameRefusedOnBuiltinFunction(PropertyName);
String messageOfPrototypeRefusedOnBuiltinFunction();
void didCreateRealm(JSGlobalObject*);
enum class DeferredWatchpointKind : bool { Structure, PropertyChange };
void willWatchBuiltins(JSGlobalObject*);
void didWatchBuiltins(JSGlobalObject*);
bool defersWatchpoints(JSGlobalObject*);
bool deferWatchpoint(JSGlobalObject*, const ObjectPropertyCondition&, InlineWatchpointSet&, DeferredWatchpointKind);
void installDeferredWatchpoints(JSGlobalObject*);
[[noreturn]] void stopBecauseRealmHasABadTime();
inline void didChangePrototypeChain(VM& vm, JSObject* changed, UniquedStringImpl* nameOrNull) { didChangeChainObject(vm, changed, nameOrNull); }
bool becomesDictionaryByAddition(VM&, JSObject*, Structure*, UniquedStringImpl*);
void didMakeStructureForAddition(VM&, Structure* made);
Structure* addBrandForSuperCall(VM&, JSFunction* newTarget, Structure*);
void finishFillingObjects(VM&);
void runFilesThatWaitInLoader(VM&);
void didStartWatchingReplacements(VM&);
void didChangeMicrotaskQueueing(VM&);
void didChangeGlobalLexicalBindings(JSGlobalObject*);
void setLoopPollsEnabled(VM&, bool);
JSObject* targetThatProxyDefinesOn(VM&, JSValue receiver);

JSArray* tryCreateArrayOfPairs(JSGlobalObject*, JSCell* items);

class NoHelperThreadsScope {
    WTF_MAKE_NONCOPYABLE(NoHelperThreadsScope);
public:
    NoHelperThreadsScope();
    ~NoHelperThreadsScope();
};
extern std::atomic<bool> wasNoHelperThreadsScopeEntered;
bool isThreadInNoHelperThreadsScope();
ALWAYS_INLINE bool isInNoHelperThreadsScope() { return wasNoHelperThreadsScopeEntered.load(std::memory_order_relaxed) && isThreadInNoHelperThreadsScope(); }

} } // namespace JSC::AOT::RuntimeHooks

#endif // ENABLE(AOT)
