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
#include "PythonPosixModule.h"

#if OS(UNIX)

#include "JSCInlines.h"
#include "PyDict.h"
#include "PyRealm.h"
#include "PyStateObject.h"
#include "PyTuple.h"
#include "PythonBuiltins.h"
#include "PythonBytes.h"
#include "PythonOperations.h"
#include "PythonPosix.h"
#include "PythonSequences.h"
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include <wtf/SafeStrerror.h>

// The module termios: Modules/termios.c of CPython. Like posix, it is for whoever embeds the engine to say whether a program is to have it.

namespace JSC { namespace Python {

#if defined(VSWTCH) && !defined(VSWTC)
#define VSWTC VSWTCH
#endif

#if defined(VSWTC) && !defined(VSWTCH)
#define VSWTCH VSWTC
#endif

struct TermiosModuleState final : NativeState {
    PYTHON_NATIVE_STATE(TermiosModuleState);
    WriteBarrier<PyType> error; // termios.error
};

template<typename Visitor>
void TermiosModuleState::visit(Visitor& visitor)
{
    visitor.append(error);
}

// PyErr_SetFromErrno(state->TermiosError). It is not an OSError, and so is made of the number and what that means and knows nothing more of either.
static EncodedJSValue raiseTermiosError(JSGlobalObject* globalObject, ThrowScope& scope)
{
    VM& vm = globalObject->vm();
    int error = errno;
    JSValue exception = call(globalObject, globalObject->pyRealm()->moduleState<TermiosModuleState>().error->object(), jsNumber(error), jsString(vm, String::fromUTF8(safeStrerror(error).span())));
    RETURN_IF_EXCEPTION(scope, { });
    setContext(globalObject, asObject(exception));
    throwException(globalObject, scope, exception);
    return { };
}

#define DESCRIPTOR_ARGUMENT() \
    auto descriptor = toFileDescriptorOrFile(globalObject, args.at(0)); \
    RETURN_IF_EXCEPTION(scope, { })

// tcgetattr(fd, /)
PYTHON_NATIVE(termiosGetAttributes)
{
    NATIVE_PROLOGUE();
    DESCRIPTOR_ARGUMENT();
    struct termios mode { };
    if (tcgetattr(*descriptor, &mode) == -1)
        return raiseTermiosError(globalObject, scope);
    speed_t inputSpeed = cfgetispeed(&mode);
    speed_t outputSpeed = cfgetospeed(&mode);

    MarkedArgumentBuffer characters;
    for (unsigned i = 0; i < NCCS; ++i) {
        uint8_t character = mode.c_cc[i];
        // In some systems these two share their places with two others, so they are only numbers when they are what is there.
        if (!(mode.c_lflag & ICANON) && (i == VMIN || i == VTIME))
            characters.append(jsNumber(character));
        else
            characters.append(newBytes(globalObject, std::span(&character, 1)));
    }
    MarkedArgumentBuffer items;
    for (auto flags : { mode.c_iflag, mode.c_oflag, mode.c_cflag, mode.c_lflag })
        items.append(intFromInt64(globalObject, static_cast<long>(flags)));
    items.append(intFromInt64(globalObject, static_cast<long>(inputSpeed)));
    items.append(intFromInt64(globalObject, static_cast<long>(outputSpeed)));
    items.append(newList(globalObject, characters));
    return JSValue::encode(newList(globalObject, items));
}

// tcsetattr(fd, when, attributes, /)
PYTHON_NATIVE(termiosSetAttributes)
{
    NATIVE_PROLOGUE();
    DESCRIPTOR_ARGUMENT();
    auto when = toCInt(globalObject, args.at(1));
    RETURN_IF_EXCEPTION(scope, { });
    JSValue attributes = args.at(2);
    auto isListOf = [&] (JSValue value, unsigned length) { return isInstance(globalObject, value, realm->typeList()) && asList(value)->length() == length; };
    if (!isListOf(attributes, 7))
        return JSValue::encode(raiseTypeError(globalObject, scope, "tcsetattr, arg 3: must be 7 element list"_s));

    // What it is now, in case there is anything in it that is not shown
    struct termios mode;
    if (tcgetattr(*descriptor, &mode) == -1)
        return raiseTermiosError(globalObject, scope);
    // What one of them has for __index__() may have made the list shorter. What is not there is None.
    long numbers[6];
    for (unsigned i = 0; i < 6; ++i) {
        auto number = toCLong(globalObject, listGet(globalObject, asList(attributes), i));
        RETURN_IF_EXCEPTION(scope, { });
        numbers[i] = *number;
    }
    mode.c_iflag = static_cast<tcflag_t>(numbers[0]);
    mode.c_oflag = static_cast<tcflag_t>(numbers[1]);
    mode.c_cflag = static_cast<tcflag_t>(numbers[2]);
    mode.c_lflag = static_cast<tcflag_t>(numbers[3]);

    JSValue characters = listGet(globalObject, asList(attributes), 6);
    if (!isListOf(characters, NCCS))
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("tcsetattr: attributes[6] must be "_s, static_cast<unsigned>(NCCS), " element list"_s)));
    for (unsigned i = 0; i < NCCS; ++i) {
        JSValue item = listGet(globalObject, asList(characters), i);
        if (typeOf(globalObject, item)->hasFlag(PyType::IsBytes) && builtinBufferOf(item)->size() == 1)
            mode.c_cc[i] = static_cast<cc_t>((*builtinBufferOf(item))[0]);
        else if (isInstance(globalObject, item, realm->typeInt())) {
            auto number = toCLong(globalObject, item);
            RETURN_IF_EXCEPTION(scope, { });
            mode.c_cc[i] = static_cast<cc_t>(*number);
        } else
            return JSValue::encode(raiseTypeError(globalObject, scope, "tcsetattr: elements of attributes must be bytes objects of length 1 or integers"_s));
    }

    if (cfsetispeed(&mode, static_cast<speed_t>(numbers[4])) == -1 || cfsetospeed(&mode, static_cast<speed_t>(numbers[5])) == -1 || tcsetattr(*descriptor, *when, &mode) == -1)
        return raiseTermiosError(globalObject, scope);
    RETURN_NONE();
}

// tcsendbreak(fd, duration, /), tcflush(fd, queue, /) and tcflow(fd, action, /)
enum class WithNumber : uint8_t { SendBreak, Flush, Flow };

PYTHON_NATIVE(termiosCallWithNumber)
{
    NATIVE_PROLOGUE();
    DESCRIPTOR_ARGUMENT();
    auto number = toCInt(globalObject, args.at(1));
    RETURN_IF_EXCEPTION(scope, { });
    int result = -1;
    switch (unpack<WithNumber>(callFrame, 0)) {
    case WithNumber::SendBreak:
        result = tcsendbreak(*descriptor, *number);
        break;
    case WithNumber::Flush:
        result = tcflush(*descriptor, *number);
        break;
    case WithNumber::Flow:
        result = tcflow(*descriptor, *number);
        break;
    }
    if (result == -1)
        return raiseTermiosError(globalObject, scope);
    RETURN_NONE();
}

// tcdrain(fd, /)
PYTHON_NATIVE(termiosDrain)
{
    NATIVE_PROLOGUE();
    DESCRIPTOR_ARGUMENT();
    if (tcdrain(*descriptor) == -1)
        return raiseTermiosError(globalObject, scope);
    RETURN_NONE();
}

// tcgetwinsize(fd, /)
PYTHON_NATIVE(termiosGetWindowSize)
{
    NATIVE_PROLOGUE();
    DESCRIPTOR_ARGUMENT();
    struct winsize size;
    if (ioctl(*descriptor, TIOCGWINSZ, &size) == -1)
        return raiseTermiosError(globalObject, scope);
    return JSValue::encode(PyTuple::create(globalObject, { jsNumber(size.ws_row), jsNumber(size.ws_col) }));
}

// tcsetwinsize(fd, winsize, /)
PYTHON_NATIVE(termiosSetWindowSize)
{
    NATIVE_PROLOGUE();
    DESCRIPTOR_ARGUMENT();
    JSValue given = args.at(1);
    bool isPair = isSequence(globalObject, given);
    if (isPair) {
        int64_t length = sequenceSize(globalObject, given);
        // Whatever is wrong with asking, this is what is said.
        if (scope.exception()) {
            if (!catchException(globalObject, BuiltinType::BaseException))
                return { };
            isPair = false;
        } else
            isPair = length == 2;
    }
    if (!isPair)
        return JSValue::encode(raiseTypeError(globalObject, scope, "tcsetwinsize, arg 2: must be a two-item sequence"_s));
    long numbers[2];
    for (unsigned i = 0; i < 2; ++i) {
        JSValue item = sequenceItem(globalObject, given, i);
        RETURN_IF_EXCEPTION(scope, { });
        auto number = toCLong(globalObject, item);
        RETURN_IF_EXCEPTION(scope, { });
        numbers[i] = *number;
    }

    // What it is now, since there is more to it than these two
    struct winsize size;
    if (ioctl(*descriptor, TIOCGWINSZ, &size) == -1)
        return raiseTermiosError(globalObject, scope);
    size.ws_row = static_cast<unsigned short>(numbers[0]);
    size.ws_col = static_cast<unsigned short>(numbers[1]);
    if (size.ws_row != numbers[0] || size.ws_col != numbers[1])
        return JSValue::encode(raise(globalObject, scope, BuiltinType::OverflowError, "winsize value(s) out of range."_s));
    if (ioctl(*descriptor, TIOCSWINSZ, &size) == -1)
        return raiseTermiosError(globalObject, scope);
    RETURN_NONE();
}

JSObject* createTermiosModule(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRealm* realm = globalObject->pyRealm();
    auto& state = realm->moduleState<TermiosModuleState>();
    if (!state.error) {
        // PyErr_NewException("termios.error", NULL, NULL)
        PyDict* contents = PyDict::create(globalObject);
        contents->setString(globalObject, "__module__"_s, jsNontrivialString(vm, "termios"_s));
        JSValue error = newType(globalObject, realm->typeType(), jsNontrivialString(vm, "error"_s), PyTuple::create(globalObject, { realm->type(BuiltinType::Exception) }), contents, nullptr);
        RETURN_IF_EXCEPTION(scope, nullptr);
        state.error.set(vm, realm, asType(error));
    }
    JSObject* module = newBuiltinModule(globalObject, "termios"_s);
    module->putDirect(vm, Identifier::fromString(vm, "error"_s), state.error->object());

    addFunction(globalObject, module, "tcgetattr"_s, termiosGetAttributes);
    addFunction(globalObject, module, "tcsetattr"_s, termiosSetAttributes);
    addFunction(globalObject, module, "tcsendbreak"_s, termiosCallWithNumber, pack(WithNumber::SendBreak));
    addFunction(globalObject, module, "tcdrain"_s, termiosDrain);
    addFunction(globalObject, module, "tcflush"_s, termiosCallWithNumber, pack(WithNumber::Flush));
    addFunction(globalObject, module, "tcflow"_s, termiosCallWithNumber, pack(WithNumber::Flow));
    addFunction(globalObject, module, "tcgetwinsize"_s, termiosGetWindowSize);
    addFunction(globalObject, module, "tcsetwinsize"_s, termiosSetWindowSize);

    // What is given to ioctl() is not less than nothing, though the system may have it as if it were.
    auto add = [&] (ASCIILiteral name, long value) {
        bool isRequest = name.length() >= 3 && name[0] == 'T' && name[1] == 'I' && name[2] == 'O';
        module->putDirect(vm, Identifier::fromString(vm, name), isRequest ? intFromUInt64(globalObject, static_cast<unsigned>(value)) : intFromInt64(globalObject, value));
    };
#define ADD_CONSTANT(name) add(#name ""_s, static_cast<long>(name))
#include "PythonTermiosConstants.h"
#undef ADD_CONSTANT
    return module;
}

} } // namespace JSC::Python

#endif // OS(UNIX)
