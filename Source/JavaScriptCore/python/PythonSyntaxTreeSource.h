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
#include "PythonASTValidator.h"
#include "SourceProvider.h"

namespace JSC {

class VM;

namespace Python {

class Arena;

// Code is made from its source when it is wanted, and can be thrown away and made again, and what a code object gives as co_code is its source. What compile() makes of a syntax tree has no source that anyone
// wrote, so its source is the tree, written out. It is read back by readSyntaxTree() and its like where the text of a program is taken apart by parse() and its like, and a function in it is a part of the text as
// a function in a program is.
//
//     node       ( class ; [where] [@ where] value... )      `class` is an ASTClass. `where` is there if the class says where it is, and for the whole
//     where      line , column , line , column ;             where it begins and where it ends
//     list       [ count : value... ]
//     None       ~
//     Load() &c  % class ;
//     int        # number ;
//     identifier n length : characters
//     string     s length : characters                       y if it is bytes, with a character for each
//     constant   N T F E                                     None, True, False and ...
//                i [-] digits ;                              an int, in hexadecimal
//                f bits                                      a float: sixteen digits, in hexadecimal
//                j bits                                      an imaginary number
//                c bits bits                                 a complex number
//                u length : characters                       a str
//                b length : characters                       bytes
//                t count : constant...                       a tuple
//                z count : constant...                       a frozenset
//
// What an instruction is from is a part of the source: where it begins and where it ends. Here that is where the node is written, and where the node says that it is comes first in it so that it can be got
// from that alone. Getting an attribute can be somewhere that is not where any node is, and that is what comes after the @.
class SyntaxTreeSourceProvider final : public StringSourceProvider {
public:
    static Ref<SyntaxTreeSourceProvider> create(const String& tree, const SourceOrigin& origin, String sourceURL)
    {
        return adoptRef(*new SyntaxTreeSourceProvider(tree, origin, WTF::move(sourceURL)));
    }

    // Where the node that is written there says that it is.
    LineColumn lineColumnInTextForOffset(unsigned offset) final;

private:
    SyntaxTreeSourceProvider(const String& tree, const SourceOrigin& origin, String&& sourceURL)
        : StringSourceProvider(tree, origin, SourceTaintedOrigin::Untainted, WTF::move(sourceURL), TextPosition(), SourceProviderSourceType::PythonSyntaxTree)
    {
    }
};

// Lines from 1, and columns from 0 in bytes of UTF-8. All nought if there is no node written there that says.
struct PlaceInSource {
    unsigned line { 0 };
    unsigned column { 0 };
    unsigned endLine { 0 };
    unsigned endColumn { 0 };
};
PlaceInSource placeOfNodeInSyntaxTree(StringView tree, unsigned offset);

// The tree has to be one that validate() has nothing to say about.
String writeSyntaxTree(VM&, Module&);

// These are to what has been written out as parse(), parseDefinition() and parseExpression() are to the text of a program. What they are given may be what a program has said is co_code, so nothing is taken on
// trust. Null if it is not a tree, or not one that could be compiled.
Module* readSyntaxTree(VM&, Arena&, StringView tree, Module::Kind);
Statement* readDefinition(VM&, Arena&, StringView tree, unsigned start, unsigned end);
Expression* readExpression(VM&, Arena&, StringView tree, unsigned start, unsigned end);

} } // namespace JSC::Python
