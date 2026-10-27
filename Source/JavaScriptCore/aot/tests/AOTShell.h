/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <span>
#include <wtf/Forward.h>
#include <wtf/text/ASCIILiteral.h>

namespace JSC {

class JSGlobalObject;
class JSPromise;
class VM;

namespace AOT {
struct ModuleHost;
}

} // namespace JSC

namespace JSC::AOT::Shell {

void enableGlobalsOfBuiltinTable();
void mapImageFromOptions();
void addShellFunctions(VM&, JSGlobalObject*);
std::span<const ASCIILiteral> shellFunctionNames();
bool isKeyOfCompiledModule(const String&);
// Null for a key that names no module of the image, or with an exception.
JSPromise* fetchCompiledModule(JSGlobalObject*, const String& key);
extern const ModuleHost moduleHost;

} // namespace JSC::AOT::Shell

#endif // ENABLE(AOT)
