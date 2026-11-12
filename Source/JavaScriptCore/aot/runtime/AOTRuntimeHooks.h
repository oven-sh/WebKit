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
class InternalFunction;
class JSFunction;
class JSValue;
class ObjectPropertyCondition;
class PropertyName;
struct CallerFrameAndPC;

namespace AOT {

enum class RejectedWrite : uint8_t { Store, Definition, Deletion, Prototype };

void didChangeChainObject(VM&, JSObject* changed, UniquedStringImpl* uidOrNull);

} // namespace AOT

namespace AOT::RuntimeHooks {

using AOT::didCreateClass;
using AOT::didCreateStaticProperty;
using AOT::didRejectWrite;
using AOT::isReportedAsReadOnly;
using AOT::lockedObjectErrorMessage;
using AOT::reflectThrowsOnLockedProperty;
using AOT::takePreallocatedStaticProperty;

void willInitializeRealm(JSGlobalObject*);
Structure* structureForHostFunctions(JSGlobalObject*, Structure*);
bool rejectsNativeConstructorProperty(JSGlobalObject*, PropertyName);
bool acceptsHasInstanceOfHost(InternalFunction*, PropertyName, JSValue method);
inline bool locksNativeConstructorPrototypes(JSGlobalObject* realm) { return locksBuiltins(realm); }
String lockedBuiltinPropertyErrorMessage(PropertyName);
String lockedBuiltinPrototypeErrorMessage();
void didCreateRealm(JSGlobalObject*);
enum class DeferredWatchpointKind : bool { Structure, PropertyChange };
void willWatchBuiltins(JSGlobalObject*);
void didWatchBuiltins(JSGlobalObject*);
bool defersWatchpoints(JSGlobalObject*);
bool deferWatchpoint(JSGlobalObject*, const ObjectPropertyCondition&, InlineWatchpointSet&, DeferredWatchpointKind);
void installDeferredWatchpoints(JSGlobalObject*);
[[noreturn]] void crashOnHavingABadTime();
[[noreturn]] void stopAtHostResult(Instance&, CallerFrameAndPC* operationFrameRecord, JSValue found, JSCell* function, uint32_t kinds);
inline void didChangePrototypeChain(VM& vm, JSObject* changed, UniquedStringImpl* nameOrNull) { didChangeChainObject(vm, changed, nameOrNull); }
bool shouldUseDictionaryForAddition(VM&, JSObject*, Structure*, UniquedStringImpl*);
void didCreateAdditionTransition(VM&, Structure* made);
Structure* addBrandForSuperCall(VM&, JSFunction* newTarget, Structure*);
void finishFillingObjects(VM&);
void transferEagerLoadsToLoader(VM&);
void didStartWatchingReplacements(VM&);
void didChangeMicrotaskQueueing(VM&);
void willRecompileRegExp(VM&);
void didChangeGlobalLexicalBindings(JSGlobalObject*);
void setLoopPollsEnabled(VM&, bool);
JSObject* proxyDefinePropertyTarget(VM&, JSValue receiver);

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
