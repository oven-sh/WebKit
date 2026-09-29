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

#include "PythonFunctionInfo.h"
#include "PythonOperators.h"
#include <optional>

namespace JSC { namespace Python {

// What comes of an operator whose operands are constants, worked out when the code is compiled. It is what CPython's Python/flowgraph.c does, and what it will not do for being too large is not done here either.
// There it is done by doing it, with the objects. Here there are no objects yet, since code is compiled for no realm in particular, so it is done with what is written down of them, and only where that is sure to come
// to what running it would: with ints of up to 127 bits, which is about as far as CPython goes with what can grow, with floats, by the functions that are used when it is run, with + and - of complex numbers, and with str, bytes and tuple. Nothing comes
// of the rest, nor of what would raise, and then it is left to be run.
using ConstantValue = CodeDetails::Constant;

std::optional<ConstantValue> foldBinaryOperation(BinaryOperator, const ConstantValue& left, const ConstantValue& right);
std::optional<ConstantValue> foldUnaryOperation(UnaryOperator, const ConstantValue&);
std::optional<ConstantValue> foldSubscript(const ConstantValue&, const ConstantValue& index);

unsigned hashOfConstant(const ConstantValue&);

// An int from 0 to 255, which CPython has an instruction for, and so does not keep among the constants.
bool isSmallInt(const ConstantValue&);
// Whether it can be one object however often the code is run. What has a bytes in it cannot: JavaScript can write to that.
bool canBeShared(const ConstantValue&);
// Whether it is an object of a realm's, and so has to be made when the code is linked, if it is to be made once.
bool isObjectOfRealm(const ConstantValue&);

} } // namespace JSC::Python
