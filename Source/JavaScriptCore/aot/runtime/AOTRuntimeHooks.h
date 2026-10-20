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
class Structure;
class VM;

namespace AOT::RuntimeHooks {

void willInitializeRealm(JSGlobalObject*);
void didCreateRealm(JSGlobalObject*);
void didCreateClass(JSGlobalObject*, const LazyClassStructure&, Structure*, JSObject* prototype, JSObject* constructorOrNull);
void didCreateStaticProperty(JSObject* holder, UniquedStringImpl* key, JSValue);
void didChangePrototypeChain(VM&, JSObject* changed, UniquedStringImpl* nameOrNull);
void didFlattenStructure(VM&, Structure*);
void didClearPristineBits(VM&);
void didChangeMicrotaskQueueing(VM&);
void didChangeGlobalLexicalBindings(JSGlobalObject*);
void setLoopPollsEnabled(VM&, bool);

} } // namespace JSC::AOT::RuntimeHooks

#endif // ENABLE(AOT)
