/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTMechanisms.h"
#include "JSExportMacros.h"
#include <optional>
#include <wtf/PrintStream.h>
#include <wtf/text/StringView.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT {

// "mechanisms=..", for testaot and the tests alone. It is no word of the schema and no option of a build: applyOptionWord() refuses it, and Bun never sees this header.
// The set goes into PreparedProgram::mechanisms, or to compileProgramWithMechanisms() and compileParsedProgramWithMechanisms() of AOTStages.h.
//   mechanisms=none   mechanisms=all   mechanisms=TypedOperations,Guesses   mechanisms=none+Folding+DirectCalls   mechanisms=-Inlining-Guesses
// From left to right. What comes first may set the whole: `none`, `all`, or the names of all that are on with commas between them, as the tests write and print a set. Without that, the list changes the set as it stands.
// `+name` adds a mechanism, and is refused while something that it leans on is withheld: so is a name between commas. `-name` withholds one, and with it all that lean on it. Defined in AOTOptionWords.cpp.
JS_EXPORT_PRIVATE bool isMechanismsWord(StringView word);
// No answer: the word was applied. Otherwise the sentence that says why not, and the set is as it was.
JS_EXPORT_PRIVATE std::optional<String> applyMechanismsWord(Typed::Mechanisms&, StringView word);
JS_EXPORT_PRIVATE ASCIILiteral nameOfMechanism(Typed::Mechanism);
// The word and a line for each name, as listOptionWords() prints its words.
JS_EXPORT_PRIVATE void listMechanismsWord(PrintStream&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
