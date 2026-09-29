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


#include "config.h"
#include "PythonBuiltins.h"

#include "JSCInlines.h"
#include "PyTuple.h"
#include "PythonOpcodeMetadata.h"
#include "PythonOperations.h"
#include "PythonSequences.h"

// The module _opcode: Modules/_opcode.c of CPython. It says what there is to know about CPython's instructions. They are not what is run here, and `co_code` is not made of them. But opcode.py and dis.py are written in
// Python over this, a good deal of the library imports those whether or not it comes to use them, and _opcode_metadata.py, which is Python too, has the numbers. So what is said about the numbers is what CPython says.

namespace JSC { namespace Python {

using namespace Opcode;

static bool isValidOpcode(int opcode) { return opcode >= 0 && opcode < static_cast<int>(std::size(metadata)) && metadata[opcode].isValid; }

// get_stack_effects() of CPython's Python/flowgraph.c. Nothing if there is no saying.
static std::optional<int> stackEffect(int opcode, int oparg, int jump)
{
    if (opcode < 0)
        return std::nullopt;
    // Not of what is a special case of another.
    if (opcode <= maximumRealOpcode && metadata[opcode].deoptimized != opcode)
        return std::nullopt;
    int popped = numberPopped(opcode, oparg);
    int pushed = numberPushed(opcode, oparg);
    if (popped < 0 || pushed < 0)
        return std::nullopt;
    if (isBlockPush(opcode) && !jump)
        return 0;
    return pushed - popped;
}

// stack_effect(opcode, oparg=None, /, *, jump=None)
PYTHON_NATIVE(opcodeStackEffect)
{
    NATIVE_PROLOGUE();
    auto opcode = toCInt(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    int oparg = 0;
    if (JSValue given = args.at(1); given && !isNone(given)) {
        // (int)PyLong_AsLong()
        auto value = toCLong(globalObject, given);
        RETURN_IF_EXCEPTION(scope, { });
        oparg = static_cast<int>(*value);
    }
    int jump = -1;
    if (JSValue given = args.at(2); given && !isNone(given)) {
        if (!given.isBoolean())
            return JSValue::encode(raiseValueError(globalObject, scope, "stack_effect: jump must be False, True or None"_s));
        jump = given.asBoolean();
    }
    auto effect = stackEffect(*opcode, oparg, jump);
    if (!effect)
        return JSValue::encode(raiseValueError(globalObject, scope, "invalid opcode or oparg"_s));
    return JSValue::encode(jsNumber(*effect));
}

// is_valid(opcode), has_arg(opcode) and the like
PYTHON_NATIVE(opcodeHas)
{
    auto flag = unpack<uint16_t>(callFrame, 0);
    NATIVE_PROLOGUE();
    auto opcode = toCInt(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsBoolean(isValidOpcode(*opcode) && (!flag || metadata[*opcode].flags & flag)));
}

PYTHON_NATIVE(opcodeHasExc)
{
    NATIVE_PROLOGUE();
    auto opcode = toCInt(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsBoolean(isValidOpcode(*opcode) && isBlockPush(*opcode)));
}

PYTHON_NATIVE(opcodeGetSpecializationStats)
{
    UNUSED_PARAM(globalObject);
    UNUSED_PARAM(callFrame);
    RETURN_NONE();
}

PYTHON_NATIVE(opcodeGetNbOps)
{
    UNUSED_PARAM(callFrame);
    VM& vm = globalObject->vm();
    MarkedArgumentBuffer items;
    for (auto& [name, symbol] : binaryOperators)
        items.append(PyTuple::create(globalObject, { jsString(vm, String(name)), jsString(vm, String(symbol)) }));
    return JSValue::encode(newList(globalObject, items));
}

enum class Names : uint8_t { UnaryIntrinsics, BinaryIntrinsics, SpecialMethods };

// get_intrinsic1_descs(), get_intrinsic2_descs() and get_special_method_names()
PYTHON_NATIVE(opcodeGetNames)
{
    VM& vm = globalObject->vm();
    std::span<const ASCIILiteral> names;
    switch (unpack<Names>(callFrame, 0)) {
    case Names::UnaryIntrinsics:
        names = unaryIntrinsics;
        break;
    case Names::BinaryIntrinsics:
        names = binaryIntrinsics;
        break;
    case Names::SpecialMethods:
        names = specialMethods;
        break;
    }
    MarkedArgumentBuffer items;
    for (ASCIILiteral name : names)
        items.append(jsString(vm, String(name)));
    return JSValue::encode(newList(globalObject, items));
}

// get_executor(code, offset)
PYTHON_NATIVE(opcodeGetExecutor)
{
    NATIVE_PROLOGUE();
    toCInt(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    if (!isCode(globalObject, args[0]))
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("expected a code object, not '"_s, typeName(globalObject, args[0]), '\'')));
    return JSValue::encode(raise(globalObject, scope, BuiltinType::RuntimeError, "Executors are not available in this build"_s));
}

JSObject* createOpcodeModule(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    JSObject* module = newBuiltinModule(globalObject, "_opcode"_s);
    addFunction(globalObject, module, "stack_effect"_s, opcodeStackEffect);
    addFunction(globalObject, module, "is_valid"_s, opcodeHas, pack(static_cast<uint16_t>(0)));
    addFunction(globalObject, module, "has_arg"_s, opcodeHas, pack(hasArg));
    addFunction(globalObject, module, "has_const"_s, opcodeHas, pack(hasConst));
    addFunction(globalObject, module, "has_name"_s, opcodeHas, pack(hasName));
    addFunction(globalObject, module, "has_jump"_s, opcodeHas, pack(hasJump));
    addFunction(globalObject, module, "has_free"_s, opcodeHas, pack(hasFree));
    addFunction(globalObject, module, "has_local"_s, opcodeHas, pack(hasLocal));
    addFunction(globalObject, module, "has_exc"_s, opcodeHasExc);
    addFunction(globalObject, module, "get_specialization_stats"_s, opcodeGetSpecializationStats);
    addFunction(globalObject, module, "get_nb_ops"_s, opcodeGetNbOps);
    addFunction(globalObject, module, "get_intrinsic1_descs"_s, opcodeGetNames, pack(Names::UnaryIntrinsics));
    addFunction(globalObject, module, "get_intrinsic2_descs"_s, opcodeGetNames, pack(Names::BinaryIntrinsics));
    addFunction(globalObject, module, "get_special_method_names"_s, opcodeGetNames, pack(Names::SpecialMethods));
    addFunction(globalObject, module, "get_executor"_s, opcodeGetExecutor);
    module->putDirect(vm, Identifier::fromString(vm, "ENABLE_SPECIALIZATION"_s), jsNumber(1));
    module->putDirect(vm, Identifier::fromString(vm, "ENABLE_SPECIALIZATION_FT"_s), jsNumber(1));
    return module;
}

} } // namespace JSC::Python
