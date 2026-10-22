/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <JavaScriptCore/JSExportMacros.h>
#include <wtf/Forward.h>

namespace JSC {

class JSGlobalObject;
class JSObject;
class JSValue;
class VM;

namespace AOT {

// A NAME NEVER THROWS. The first read of some members of the host runs code of the library: `Bun.$`, `console.Console`, `process.stdin`. The image holds that code only for a program that uses the member,
// and whoever goes over the members of the object without naming one reads it all the same: Object.assign(), a spread, `holder[key]`. Such a read answers a function with the name of the member.
// Only a call or a construction of that function throws the error that says which built-in is not in the executable.

// The error is about to be thrown because code of the library that is not in the image was to run.
void noteErrorOfCodeLeftOut(JSGlobalObject*, JSObject* error);
// Where the host was making or getting a member and an exception is pending. If it is that error, it is taken and the function is answered. For any other exception the answer is empty and nothing happens.
// Whoever puts the answer into a slot of the holder passes no object. Where native code answers for the name at each read, it passes the holder, and gets the same function each time.
JS_EXPORT_PRIVATE JSValue takeStandInForMemberLeftOut(JSGlobalObject*, const String& nameOfMember, JSObject* answeredBy = nullptr);
// The same for a getter of the host that needs code of the library to make what it answers (`require.main`). A getter that the program has written keeps what it throws.
// READING AN ACCESSOR IS CALLING ITS GETTER: where the function of the library behind an accessor is itself left out (Buffer.prototype.offset), the read throws: what it answers may be a number,
// and a function in the place of a number is a wrong answer. throwLibraryFunctionNotCompiled() does not note that error.
JSValue takeStandInForGetterLeftOut(JSGlobalObject*, JSObject* getter);
// The error says that what was read from `base` is no function. Where the real member is an object (process.stdout), the program reads `process.stdout.write is not a function`, which explains nothing.
// If `base` is a function that stands for a member, the message goes on with the sentence that a call of that function throws. Only the text changes, of an error that is thrown anyway.
void explainErrorBehindMemberLeftOut(JSGlobalObject*, JSObject* error, JSValue base);
void forgetErrorOfCodeLeftOut(VM&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
