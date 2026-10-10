/*
 * Copyright (C) 2016 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include "ConstructAbility.h"
#include "ConstructorKind.h"
#include "ImplementationVisibility.h"
#include "InlineAttribute.h"
#include "ParserModes.h"
#include "SourceCode.h"

namespace JSC {

class ParserError;

JS_EXPORT_PRIVATE UnlinkedFunctionExecutable* createBuiltinExecutable(VM&, const SourceCode&, const Identifier&, ImplementationVisibility, ConstructorKind, ConstructAbility, InlineAttribute);
// For builtin source that this VM may not be able to parse: another build's, when an embedder generates bytecode for
// another executable. createBuiltinExecutable crashes on a source that does not parse, in the builds and under the
// option that make it parse the source (assertions on, validateBytecode). There this returns null and the error.
// Elsewhere nothing is parsed here, and generating the code reports the error.
JS_EXPORT_PRIVATE UnlinkedFunctionExecutable* tryCreateBuiltinExecutable(VM&, const SourceCode&, const Identifier&, ImplementationVisibility, ConstructorKind, ConstructAbility, InlineAttribute, ParserError&);

} // namespace JSC
