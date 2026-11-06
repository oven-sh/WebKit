/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFunctionData.h"
#include "AOTRecordTypes.h"
#include "JSExportMacros.h"
#include "LineColumn.h"
#include <optional>
#include <wtf/text/WTFString.h>

namespace JSC {

class Identifier;
class JSGlobalObject;
class JSObject;
class JSString;
class VM;

namespace AOT {

class FunctionDescription;
struct State;

// CalculatedDisplayName: what getCalculatedDisplayName() answers for the function object, which is the name in the lines `name@url:line:column` that the engine writes itself.
enum class NameForm : uint8_t { AsWritten, InFrames, CalculatedDisplayName };

// callee: the function object that runs in the frame, where the frame has recorded it. Nobody has seen one that lies in a frame, so it has no name but that of its function.
JS_EXPORT_PRIVATE String functionName(VM&, const FunctionDescription&, NameForm, JSObject* callee = nullptr, bool calleeLiesInFrame = false);
JSString* functionSourceText(JSGlobalObject*, FunctionID);
// UTF-8. Empty where the executable does not hold the text.
std::span<const Latin1Character> functionSourceBytes(FunctionID);
std::span<const Latin1Character> functionSourceBytesOfSource(FunctionID);
// The lines of that text above the place where the function begins: none but for the constructor of a class.
unsigned linesOfTextAboveSource(FunctionID);
// Where the line and the column of the file of the module lie in the text that is kept, counted from the start of sourceBytesOfAllFunctions().
// What is searched around the place is the text of the function. The body of a module has none: there it is the piece of the file that holds the place.
struct PlaceInKeptText {
    size_t startOfSearched;
    size_t endOfSearched;
    size_t at;
    // The piece of the file that holds it all. What is printed in place of a part of it is a copy of nothing outside it.
    size_t startOfPiece;
    size_t endOfPiece;
};
std::optional<PlaceInKeptText> findInKeptText(FunctionID, ModuleID, LineColumn);
// The text of every function, which functionSourceBytes() answers a part of.
std::span<const Latin1Character> sourceBytesOfAllFunctions();
// What `bun run` has in place of pieces of the text of the module. Nothing where the image counts from elsewhere than the caller.
enum class StartOfPrintedText : uint8_t { SourceBytesOfAllFunctions, File };
Vector<TextPrintedInPlace> textPrintedInPlaceOf(ModuleID, StartOfPrintedText);
// Those that start in text[from, to), or the one that says that the file runs as it is written.
Vector<TextPrintedInPlace> textPrintedInPlaceOf(ModuleID, StartOfPrintedText, size_t from, size_t to);

String writtenFunctionName(FunctionID);
String initialNameProperty(FunctionID);
JSString* initialNamePropertyString(VM&, State&, FunctionID);

void setComputedFunctionName(VM&, JSObject* closure, const Identifier& key, MethodKind);


void didDefineNameProperty(VM&, JSObject* closure, const String& name);
void didDeleteNameProperty(VM&, JSObject* closure);
// The engine makes `name` a property of the function object when somebody first looks for it there: a read, a test, a list of the own keys, a definition.
void didLookForNameProperty(VM&, JSObject* closure);
// A property `name` that was stored without a look for it: the first look puts the function's own name in its place. Frames show the stored one until then.
void showNameUntilLookedFor(VM&, JSObject* closure, const String& name);
// Behind whatever may have changed the own property `displayName` of the function object: a store, a definition, a deletion.
void didChangeDisplayName(VM&, JSObject* closure);

} // namespace AOT
} // namespace JSC

#endif // ENABLE(AOT)
