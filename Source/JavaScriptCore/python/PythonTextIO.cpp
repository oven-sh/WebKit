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
#include "PythonIO.h"

#include "PythonCharacters.h"

// io.IncrementalNewlineDecoder and io.TextIOWrapper: Modules/_io/textio.c of CPython.

namespace JSC { namespace Python {

// ---- IncrementalNewlineDecoder

namespace {

struct NewlineDecoderState final : NativeState {
    PYTHON_NATIVE_STATE(NewlineDecoderState);

    enum Seen : uint8_t { CR = 1, LF = 2, CRLF = 4, All = CR | LF | CRLF };

    WriteBarrier<Unknown> decoder;
    WriteBarrier<Unknown> errors; // Empty until __init__() has been called.
    bool hasPendingCR { false };
    bool translates { false };
    uint8_t seen { 0 };
};

template<typename Visitor>
void NewlineDecoderState::visit(Visitor& visitor)
{
    visitor.append(decoder);
    visitor.append(errors);
}

bool checkIsInitialized(JSGlobalObject* globalObject, ThrowScope& scope, NewlineDecoderState& state)
{
    if (state.errors)
        return true;
    raiseValueError(globalObject, scope, "IncrementalNewlineDecoder.__init__() not called"_s);
    return false;
}

// check_decoded()
bool checkIsDecoded(JSGlobalObject* globalObject, ThrowScope& scope, JSValue decoded)
{
    if (stringIn(decoded))
        return true;
    raiseTypeError(globalObject, scope, concatenate("decoder should return a string result, not '"_s, typeName(globalObject, decoded), '\''));
    return false;
}

// What is seen of line endings in some text, and the text with each of them made "\n" if that is wanted. They are all characters that are one code unit, so it makes no difference what else is in it.
template<typename Character>
String scanNewlines(std::span<const Character> in, bool translates, uint8_t& seen)
{
    using Seen = NewlineDecoderState::Seen;
    Vector<Character> out;
    if (translates)
        out.reserveInitialCapacity(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        Character c = in[i];
        if (c == '\n')
            seen |= Seen::LF;
        else if (c == '\r') {
            if (i + 1 < in.size() && in[i + 1] == '\n') {
                seen |= Seen::CRLF;
                ++i;
                if (!translates)
                    continue;
            } else
                seen |= Seen::CR;
            c = '\n';
        }
        if (translates)
            out.append(c);
        else if (seen == Seen::All)
            break;
    }
    return translates ? String(out.span()) : String();
}

} // anonymous namespace

JSValue decodeNewlines(JSGlobalObject* globalObject, JSValue self, JSValue input, bool isFinal)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    using Seen = NewlineDecoderState::Seen;
    auto& state = stateOf<NewlineDecoderState>(self);
    if (!checkIsInitialized(globalObject, scope, state))
        return { };

    // What is given is decoded, along with any "\r" that was kept back last time.
    JSValue output = input;
    if (!isNone(state.decoder.get())) {
        output = callMethodNamed(globalObject, state.decoder.get(), vm.pythonNames().attribute_decode, input, jsBoolean(isFinal));
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (!checkIsDecoded(globalObject, scope, output))
        return { };
    String text = stringIn(output)->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    bool isChanged = false;
    if (state.hasPendingCR && (isFinal || !text.isEmpty())) {
        text = textOrMemoryError(globalObject, tryMakeString('\r', text));
        RETURN_IF_EXCEPTION(scope, { });
        state.hasPendingCR = false;
        isChanged = true;
    }
    // A "\r" at the end is kept back even if nothing is being translated, so that readline() gets "\r\n" all at once.
    if (!isFinal && !text.isEmpty() && text[text.length() - 1] == '\r') {
        text = text.left(text.length() - 1);
        state.hasPendingCR = true;
        isChanged = true;
    }
    auto finish = [&] () -> JSValue {
        // A str, and not an instance of a class derived from it, only if there was something to do.
        return isChanged ? strOrMemoryError(globalObject, text) : output;
    };
    if (text.isEmpty())
        RELEASE_AND_RETURN(scope, finish());

    uint8_t seen = state.seen;
    bool hasNoCR = (seen == Seen::LF || !seen) && !text.contains('\r');
    if (hasNoCR) {
        if (!seen && text.contains('\n'))
            seen |= Seen::LF;
    } else if (!state.translates) {
        if (seen != Seen::All) {
            if (text.is8Bit())
                scanNewlines(text.span8(), false, seen);
            else
                scanNewlines(text.span16(), false, seen);
        }
    } else {
        text = text.is8Bit() ? scanNewlines(text.span8(), true, seen) : scanNewlines(text.span16(), true, seen);
        isChanged = true;
    }
    state.seen |= seen;
    RELEASE_AND_RETURN(scope, finish());
}

// IncrementalNewlineDecoder(decoder, translate, errors='strict')
PYTHON_NATIVE(newlineDecoderInit)
{
    NATIVE_PROLOGUE();
    JSCell* self = args[0].asCell();
    auto& state = stateOf<NewlineDecoderState>(self);
    bool translates = isTrue(globalObject, args.at(2));
    RETURN_IF_EXCEPTION(scope, { });
    JSValue errors = args.at(3);
    state.errors.set(vm, self, errors ? errors : JSValue(jsNontrivialString(vm, "strict"_s)));
    state.decoder.set(vm, self, args.at(1));
    state.translates = translates;
    state.seen = 0;
    state.hasPendingCR = false;
    RETURN_NONE();
}

PYTHON_NATIVE(newlineDecoderDecode)
{
    NATIVE_PROLOGUE();
    bool isFinal = false;
    if (JSValue value = args.at(2)) {
        isFinal = isTrue(globalObject, value);
        RETURN_IF_EXCEPTION(scope, { });
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(decodeNewlines(globalObject, args[0], args.at(1), isFinal)));
}

// PyArg_ParseTuple(state, "OK;message", &buffer, &flag). False if it raised.
static bool parseDecoderState(JSGlobalObject* globalObject, ThrowScope& scope, JSValue state, ASCIILiteral message, JSValue& buffer, uint64_t& flag)
{
    PyTuple* tuple = asTuple(state);
    if (tuple->length() != 2 || !isInstance(globalObject, tuple->at(1), globalObject->pyRealm()->typeInt())) {
        raiseTypeError(globalObject, scope, message);
        return false;
    }
    buffer = tuple->at(0);
    flag = lowBitsOfInt(tuple->at(1));
    return true;
}

PYTHON_NATIVE(newlineDecoderGetState)
{
    NATIVE_PROLOGUE();
    auto& state = stateOf<NewlineDecoderState>(args[0]);
    if (!checkIsInitialized(globalObject, scope, state))
        return { };
    JSValue buffer;
    uint64_t flag = 0;
    if (!isNone(state.decoder.get())) {
        JSValue inner = callMethodNamed(globalObject, state.decoder.get(), names.attribute_getstate);
        RETURN_IF_EXCEPTION(scope, { });
        if (!isInstance(globalObject, inner, realm->typeTuple()))
            return JSValue::encode(raiseTypeError(globalObject, scope, "illegal decoder state"_s));
        if (!parseDecoderState(globalObject, scope, inner, "illegal decoder state"_s, buffer, flag))
            return { };
    } else {
        buffer = newBytes(globalObject, { });
        RETURN_IF_EXCEPTION(scope, { });
    }
    flag <<= 1;
    if (state.hasPendingCR)
        flag |= 1;
    RELEASE_AND_RETURN(scope, JSValue::encode(PyTuple::create(globalObject, { buffer, intFromUInt64(globalObject, flag) })));
}

PYTHON_NATIVE(newlineDecoderSetState)
{
    NATIVE_PROLOGUE();
    auto& state = stateOf<NewlineDecoderState>(args[0]);
    if (!checkIsInitialized(globalObject, scope, state))
        return { };
    if (!isInstance(globalObject, args[1], realm->typeTuple()))
        return JSValue::encode(raiseTypeError(globalObject, scope, "state argument must be a tuple"_s));
    JSValue buffer;
    uint64_t flag;
    if (!parseDecoderState(globalObject, scope, args[1], "setstate(): illegal state argument"_s, buffer, flag))
        return { };
    state.hasPendingCR = flag & 1;
    flag >>= 1;
    if (isNone(state.decoder.get()))
        RETURN_NONE();
    RELEASE_AND_RETURN(scope, JSValue::encode(callMethodNamed(globalObject, state.decoder.get(), names.attribute_setstate, PyTuple::create(globalObject, { buffer, intFromUInt64(globalObject, flag) }))));
}

PYTHON_NATIVE(newlineDecoderReset)
{
    NATIVE_PROLOGUE();
    auto& state = stateOf<NewlineDecoderState>(args[0]);
    if (!checkIsInitialized(globalObject, scope, state))
        return { };
    state.seen = 0;
    state.hasPendingCR = false;
    if (isNone(state.decoder.get()))
        RETURN_NONE();
    RELEASE_AND_RETURN(scope, JSValue::encode(callMethodNamed(globalObject, state.decoder.get(), names.attribute_reset)));
}

static JSValue getNewlinesSeen(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    using Seen = NewlineDecoderState::Seen;
    auto& state = stateOf<NewlineDecoderState>(self);
    if (!checkIsInitialized(globalObject, scope, state))
        return { };
    MarkedArgumentBuffer seen;
    if (state.seen & Seen::CR)
        seen.append(jsString(vm, String("\r"_s)));
    if (state.seen & Seen::LF)
        seen.append(jsString(vm, String("\n"_s)));
    if (state.seen & Seen::CRLF)
        seen.append(jsString(vm, String("\r\n"_s)));
    if (seen.isEmpty())
        return jsUndefined();
    if (seen.size() == 1)
        return seen.at(0);
    RELEASE_AND_RETURN(scope, PyTuple::createFromArguments(globalObject, seen));
}

// ---- Finding where a line ends

template<typename Character>
std::optional<size_t> findLineEnding(const LineEndings& endings, std::span<const Character> text, size_t& consumed)
{
    auto findFrom = [&] (size_t from, Character wanted) -> size_t {
        for (size_t i = from; i < text.size(); ++i) {
            if (text[i] == wanted)
                return i;
        }
        return notFound;
    };
    if (endings.isTranslated) {
        size_t found = findFrom(0, '\n');
        if (found != notFound)
            return found + 1;
        consumed = text.size();
        return std::nullopt;
    }
    if (endings.isUniversal) {
        // The decoder sees to it that "\r\n" is not in two pieces.
        for (size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '\n')
                return i + 1;
            if (text[i] == '\r')
                return i + 1 < text.size() && text[i + 1] == '\n' ? i + 2 : i + 1;
        }
        consumed = text.size();
        return std::nullopt;
    }
    auto& newline = endings.readNewline;
    size_t length = newline.length();
    if (length == 1) {
        size_t found = findFrom(0, static_cast<Character>(newline[0]));
        if (found != notFound)
            return found + 1;
        consumed = text.size();
        return std::nullopt;
    }
    // Where the last place is that all of it could begin.
    size_t limit = text.size() >= length - 1 ? text.size() - (length - 1) : 0;
    for (size_t from = 0; from < limit;) {
        size_t found = findFrom(from, static_cast<Character>(newline[0]));
        if (found == notFound || found >= limit)
            break;
        size_t matched = 1;
        while (matched < length && text[found + matched] == newline[matched])
            ++matched;
        if (matched == length)
            return found + length;
        from = found + 1;
    }
    // The beginning of one may be at the end, and the rest of it to come.
    size_t found = findFrom(limit, static_cast<Character>(newline[0]));
    consumed = found == notFound ? text.size() : found;
    return std::nullopt;
}

template std::optional<size_t> findLineEnding(const LineEndings&, std::span<const Latin1Character>, size_t&);
template std::optional<size_t> findLineEnding(const LineEndings&, std::span<const char16_t>, size_t&);
template std::optional<size_t> findLineEnding(const LineEndings&, std::span<const char32_t>, size_t&);

void initializeTextIO(JSGlobalObject* globalObject, IOModuleState& io)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    using Kind = PyNativeFunction::Kind;

    PyType* decoder = createBuiltinType(globalObject, "_io.IncrementalNewlineDecoder"_s, realm->typeObject(), PyType::Layout::Native, PyType::IsBaseType);
    io.incrementalNewlineDecoder.set(vm, realm, decoder);
    decoder->setInstanceStructure(vm, PyStateObject::createStructure(vm, globalObject, decoder));
    decoder->setAllocator([] (VM& vm, Structure* structure) -> JSObject* { return PyStateObject::create(vm, structure, makeUnique<NewlineDecoderState>()); });
    addMethods(globalObject, decoder, {
        { "__init__"_s, newlineDecoderInit, Kind::Wrapper, 0, { }, PyNativeFunction::Arguments::AreThoseOfTheClass },
        { "decode"_s, newlineDecoderDecode },
        { "getstate"_s, newlineDecoderGetState },
        { "setstate"_s, newlineDecoderSetState },
        { "reset"_s, newlineDecoderReset },
    });
    addGetSet(globalObject, decoder, "newlines"_s, getNewlinesSeen);
}

} } // namespace JSC::Python
