/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
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

#include "PythonAST.h"
#include <wtf/text/WTFString.h>

namespace JSC {

class VM;

namespace Python {

// What is wrong with a tree that the parser did not make.
struct ASTError {
    enum class Kind : uint8_t { ValueError, TypeError, SystemError, RecursionError, RuntimeError };

    explicit operator bool() const { return !message.isNull(); }

    Kind kind { Kind::ValueError };
    String message;
};

// _PyAST_Validate(). What generates code takes it that a tree is such as the parser makes, and this is what sees to it that one that came from somewhere else is near enough: that what is assigned to says so,
// that there is something in what may not be empty, and so on. Where a node is, is an int for this, though it be kept as unsigned: it may be less than nothing.
ASTError validate(VM&, Module&);
ASTError validate(VM&, Statement&);
ASTError validate(VM&, Expression&);

} } // namespace JSC::Python
