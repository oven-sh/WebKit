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
#include "PyDict.h"
#include "PyRealm.h"
#include "PyStateObject.h"
#include "PyTuple.h"
#include "PythonBytes.h"
#include "PythonNumbers.h"
#include "PythonOperations.h"

// The module binascii: Modules/binascii.c of CPython.

namespace JSC { namespace Python {

namespace {

struct BinasciiModuleState final : NativeState {
    PYTHON_NATIVE_STATE(BinasciiModuleState);
    WriteBarrier<PyType> error;
    WriteBarrier<PyType> incomplete;
};

template<typename Visitor>
void BinasciiModuleState::visit(Visitor& visitor)
{
    visitor.append(error);
    visitor.append(incomplete);
}

// binascii.Error
JSValue raiseBinasciiError(JSGlobalObject* globalObject, ThrowScope& scope, const String& message)
{
    JSObject* exception = createException(globalObject, globalObject->pyRealm()->moduleState<BinasciiModuleState>().error.get(), jsString(globalObject->vm(), message));
    RETURN_IF_EXCEPTION(scope, { });
    setContext(globalObject, exception);
    throwException(globalObject, scope, exception);
    return { };
}

using Bytes = Vector<uint8_t, 128>;

// ascii_buffer_converter(): the bytes of a str that is all ASCII, or of whatever has bytes. They are copied, since what other arguments there are have yet to be looked at, and that can run anything. False if it raised.
bool toASCIIBuffer(JSGlobalObject* globalObject, JSValue value, Bytes& result)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (JSString* string = stringIn(value)) {
        auto view = string->view(globalObject);
        RETURN_IF_EXCEPTION(scope, false);
        if (!view->containsOnlyASCII()) {
            raiseValueError(globalObject, scope, "string argument should contain only ASCII characters"_s);
            return false;
        }
        if (!result.tryReserveCapacity(view->length())) {
            raiseMemoryError(globalObject, scope);
            return false;
        }
        for (unsigned i = 0; i < view->length(); ++i)
            result.append(static_cast<uint8_t>(view.data[i]));
        return true;
    }
    Buffer buffer = bufferOrNothing(globalObject, value);
    if (!buffer) {
        raiseTypeError(globalObject, scope, concatenate("argument should be bytes, buffer or ASCII string, not '"_s, typeName(globalObject, value), '\''));
        return false;
    }
    if (!result.tryAppend(buffer.span())) {
        raiseMemoryError(globalObject, scope);
        return false;
    }
    return true;
}

// `Py_buffer` of Argument Clinic, copied for the same reason. False if it raised.
bool toBuffer(JSGlobalObject* globalObject, JSValue value, Bytes& result)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    Buffer buffer = bufferOf(globalObject, value);
    RETURN_IF_EXCEPTION(scope, false);
    if (!result.tryAppend(buffer.span())) {
        raiseMemoryError(globalObject, scope);
        return false;
    }
    return true;
}

// `bool` of Argument Clinic. Nothing if it raised.
std::optional<bool> toBoolArgument(JSGlobalObject* globalObject, JSValue value, bool defaultValue)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (!value)
        return defaultValue;
    bool truth = isTrue(globalObject, value);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    return truth;
}

// `unsigned_int(bitwise=True)` of Argument Clinic: PyLong_AsUnsignedLongMask(). Nothing if it raised.
std::optional<uint32_t> toUnsignedIntMask(JSGlobalObject* globalObject, JSValue value)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    JSValue integer = toInt(globalObject, value);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    return static_cast<uint32_t>(lowBitsOfInt(integer));
}

JSValue bytesOrMemoryError(JSGlobalObject* globalObject, const Bytes& bytes) { return newBytes(globalObject, bytes.span()); }

constexpr auto base64Alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"_span;

// table_a2b_base64: what each character stands for. 255 is that it stands for nothing. The one that pads stands for 0.
constexpr auto base64Values = [] {
    std::array<uint8_t, 256> table;
    table.fill(255);
    for (unsigned i = 0; i < 64; ++i)
        table[static_cast<uint8_t>(base64Alphabet[i])] = static_cast<uint8_t>(i);
    table['='] = 0;
    return table;
}();

// crctab_hqx: CRC-CCITT, of which the polynomial is x^16 + x^12 + x^5 + 1
constexpr auto crcTableHQX = [] {
    std::array<uint16_t, 256> table { };
    for (unsigned i = 0; i < 256; ++i) {
        unsigned crc = i << 8;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = crc & 0x8000 ? (crc << 1) ^ 0x1021 : crc << 1;
        table[i] = static_cast<uint16_t>(crc);
    }
    return table;
}();

// crc_32_tab: the CRC-32 of ISO 3309, taken backwards
constexpr auto crcTable32 = [] {
    std::array<uint32_t, 256> table { };
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t crc = i;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = crc & 1 ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
        table[i] = crc;
    }
    return table;
}();

static_assert(crcTableHQX[1] == 0x1021 && crcTableHQX[255] == 0x1ef0);
static_assert(crcTable32[1] == 0x77073096u && crcTable32[255] == 0x2d02ef8du);

} // anonymous namespace

// a2b_uu(data, /)
PYTHON_NATIVE(binasciiA2BUU)
{
    NATIVE_PROLOGUE();
    Bytes data;
    if (!toASCIIBuffer(globalObject, args[0], data))
        return { };
    if (data.isEmpty())
        return JSValue::encode(raiseBinasciiError(globalObject, scope, "Missing length byte"_s));
    // The first says how many bytes there are.
    unsigned remaining = (data[0] - ' ') & 077;
    size_t i = 1;
    Bytes result;
    unsigned leftBits = 0;
    unsigned leftChar = 0;
    for (; remaining; ++i) {
        // Where there is no more, or the line ends, it is taken that spaces have been lost from the end of it.
        uint8_t c = i < data.size() ? data[i] : 0;
        if (c == '\n' || c == '\r' || i >= data.size())
            c = 0;
        else {
            // One more than might be expected, since there are those that write ` for nothing and not a space.
            if (c < ' ' || c > ' ' + 64)
                return JSValue::encode(raiseBinasciiError(globalObject, scope, "Illegal char"_s));
            c = (c - ' ') & 077;
        }
        leftChar = (leftChar << 6) | c;
        leftBits += 6;
        if (leftBits >= 8) {
            leftBits -= 8;
            result.append(static_cast<uint8_t>(leftChar >> leftBits));
            leftChar &= (1 << leftBits) - 1;
            --remaining;
        }
    }
    for (; i < data.size(); ++i) {
        uint8_t c = data[i];
        if (c != ' ' && c != ' ' + 64 && c != '\n' && c != '\r')
            return JSValue::encode(raiseBinasciiError(globalObject, scope, "Trailing garbage"_s));
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(bytesOrMemoryError(globalObject, result)));
}

// b2a_uu(data, /, *, backtick=False)
PYTHON_NATIVE(binasciiB2AUU)
{
    NATIVE_PROLOGUE();
    Bytes data;
    if (!toBuffer(globalObject, args[0], data))
        return { };
    auto backtick = toBoolArgument(globalObject, args.at(1), false);
    RETURN_IF_EXCEPTION(scope, { });
    if (data.size() > 45)
        return JSValue::encode(raiseBinasciiError(globalObject, scope, "At most 45 bytes at once"_s));
    Bytes result;
    auto append = [&] (unsigned value) { result.append(static_cast<uint8_t>(*backtick && !value ? '`' : value + ' ')); };
    append(static_cast<unsigned>(data.size()));
    unsigned leftBits = 0;
    unsigned leftChar = 0;
    for (size_t i = 0; i < data.size() || leftBits; ++i) {
        // What there is, and then nothing, to make it up to a whole number
        leftChar = (leftChar << 8) | (i < data.size() ? data[i] : 0);
        leftBits += 8;
        while (leftBits >= 6) {
            leftBits -= 6;
            append((leftChar >> leftBits) & 0x3f);
        }
    }
    result.append('\n');
    RELEASE_AND_RETURN(scope, JSValue::encode(bytesOrMemoryError(globalObject, result)));
}

// a2b_base64(data, /, *, strict_mode=False)
PYTHON_NATIVE(binasciiA2BBase64)
{
    NATIVE_PROLOGUE();
    Bytes data;
    if (!toASCIIBuffer(globalObject, args[0], data))
        return { };
    auto isStrict = toBoolArgument(globalObject, args.at(1), false);
    RETURN_IF_EXCEPTION(scope, { });
    Bytes result;
    if (!result.tryReserveCapacity((data.size() + 3) / 4 * 3))
        return JSValue::encode(raiseMemoryError(globalObject, scope));
    unsigned quadPosition = 0;
    uint8_t leftChar = 0;
    unsigned pads = 0;
    for (size_t i = 0; i < data.size(); ++i) {
        uint8_t c = data[i];
        if (c == '=') {
            ++pads;
            if (quadPosition >= 2 && quadPosition + pads <= 4)
                continue;
            // RFC 4648, 3.3: padding that is where none should be may be passed over, and so may more than there should be.
            if (!*isStrict)
                continue;
            if (quadPosition == 1)
                break; // It is said below what is wrong.
            return JSValue::encode(raiseBinasciiError(globalObject, scope, !quadPosition && !i ? "Leading padding not allowed"_s : "Excess padding not allowed"_s));
        }
        c = base64Values[c];
        if (c >= 64) {
            if (*isStrict)
                return JSValue::encode(raiseBinasciiError(globalObject, scope, "Only base64 data is allowed"_s));
            continue;
        }
        if (pads && *isStrict)
            return JSValue::encode(raiseBinasciiError(globalObject, scope, quadPosition + pads == 4 ? "Excess data after padding"_s : "Discontinuous padding not allowed"_s));
        pads = 0;
        switch (quadPosition) {
        case 0:
            quadPosition = 1;
            leftChar = c;
            break;
        case 1:
            quadPosition = 2;
            result.append(static_cast<uint8_t>((leftChar << 2) | (c >> 4)));
            leftChar = c & 0x0f;
            break;
        case 2:
            quadPosition = 3;
            result.append(static_cast<uint8_t>((leftChar << 4) | (c >> 2)));
            leftChar = c & 0x03;
            break;
        case 3:
            quadPosition = 0;
            result.append(static_cast<uint8_t>((leftChar << 6) | c));
            leftChar = 0;
            break;
        }
    }
    // One over is what nothing could have been encoded as.
    if (quadPosition == 1)
        return JSValue::encode(raiseBinasciiError(globalObject, scope, concatenate("Invalid base64-encoded string: number of data characters ("_s, result.size() / 3 * 4 + 1, ") cannot be 1 more than a multiple of 4"_s)));
    if (quadPosition && quadPosition + pads < 4)
        return JSValue::encode(raiseBinasciiError(globalObject, scope, "Incorrect padding"_s));
    RELEASE_AND_RETURN(scope, JSValue::encode(bytesOrMemoryError(globalObject, result)));
}

// b2a_base64(data, /, *, newline=True)
PYTHON_NATIVE(binasciiB2ABase64)
{
    NATIVE_PROLOGUE();
    Bytes data;
    if (!toBuffer(globalObject, args[0], data))
        return { };
    auto newline = toBoolArgument(globalObject, args.at(1), true);
    RETURN_IF_EXCEPTION(scope, { });
    Bytes result;
    if (!result.tryReserveCapacity((data.size() + 2) / 3 * 4 + 1))
        return JSValue::encode(raiseMemoryError(globalObject, scope));
    unsigned leftBits = 0;
    unsigned leftChar = 0;
    for (uint8_t byte : data) {
        leftChar = (leftChar << 8) | byte;
        leftBits += 8;
        while (leftBits >= 6) {
            leftBits -= 6;
            result.append(static_cast<uint8_t>(base64Alphabet[(leftChar >> leftBits) & 0x3f]));
        }
    }
    if (leftBits == 2) {
        result.append(static_cast<uint8_t>(base64Alphabet[(leftChar & 3) << 4]));
        result.append('=');
        result.append('=');
    } else if (leftBits == 4) {
        result.append(static_cast<uint8_t>(base64Alphabet[(leftChar & 0xf) << 2]));
        result.append('=');
    }
    if (*newline)
        result.append('\n');
    RELEASE_AND_RETURN(scope, JSValue::encode(bytesOrMemoryError(globalObject, result)));
}

// crc_hqx(data, crc, /)
PYTHON_NATIVE(binasciiCRCHQX)
{
    NATIVE_PROLOGUE();
    Bytes data;
    if (!toBuffer(globalObject, args[0], data))
        return { };
    auto given = toUnsignedIntMask(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    unsigned crc = *given & 0xffff;
    for (uint8_t byte : data)
        crc = ((crc << 8) & 0xff00) ^ crcTableHQX[(crc >> 8) ^ byte];
    return JSValue::encode(intFromUInt64(globalObject, crc));
}

// crc32(data, crc=0, /)
PYTHON_NATIVE(binasciiCRC32)
{
    NATIVE_PROLOGUE();
    Bytes data;
    if (!toBuffer(globalObject, args[0], data))
        return { };
    uint32_t crc = 0;
    if (JSValue value = args.at(1)) {
        auto given = toUnsignedIntMask(globalObject, value);
        RETURN_IF_EXCEPTION(scope, { });
        crc = *given;
    }
    crc = ~crc;
    for (uint8_t byte : data)
        crc = crcTable32[(crc ^ byte) & 0xff] ^ (crc >> 8);
    return JSValue::encode(intFromUInt64(globalObject, static_cast<uint32_t>(~crc)));
}

// b2a_hex(data, sep=<unrepresentable>, bytes_per_sep=1), which is hexlify() as well
PYTHON_NATIVE(binasciiB2AHex)
{
    NATIVE_PROLOGUE();
    Buffer buffer = bufferOf(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    String text = hexOfBuffer(globalObject, args, buffer);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(newBytes(globalObject, text.span8())));
}

// a2b_hex(hexstr, /), which is unhexlify() as well
PYTHON_NATIVE(binasciiA2BHex)
{
    NATIVE_PROLOGUE();
    Bytes data;
    if (!toASCIIBuffer(globalObject, args[0], data))
        return { };
    if (data.size() % 2)
        return JSValue::encode(raiseBinasciiError(globalObject, scope, "Odd-length string"_s));
    Bytes result;
    if (!result.tryReserveCapacity(data.size() / 2))
        return JSValue::encode(raiseMemoryError(globalObject, scope));
    for (size_t i = 0; i < data.size(); i += 2) {
        if (!isASCIIHexDigit(data[i]) || !isASCIIHexDigit(data[i + 1]))
            return JSValue::encode(raiseBinasciiError(globalObject, scope, "Non-hexadecimal digit found"_s));
        result.append(static_cast<uint8_t>(toASCIIHexValue(data[i], data[i + 1])));
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(bytesOrMemoryError(globalObject, result)));
}

// a2b_qp(data, header=False)
PYTHON_NATIVE(binasciiA2BQP)
{
    NATIVE_PROLOGUE();
    Bytes data;
    if (!toASCIIBuffer(globalObject, args[0], data))
        return { };
    auto isHeader = toBoolArgument(globalObject, args.at(1), false);
    RETURN_IF_EXCEPTION(scope, { });
    Bytes result;
    if (!result.tryReserveCapacity(data.size()))
        return JSValue::encode(raiseMemoryError(globalObject, scope));
    size_t length = data.size();
    for (size_t in = 0; in < length;) {
        if (data[in] == '=') {
            ++in;
            if (in >= length)
                break;
            if (data[in] == '\n' || data[in] == '\r') {
                // The line goes on.
                if (data[in] != '\n') {
                    while (in < length && data[in] != '\n')
                        ++in;
                }
                if (in < length)
                    ++in;
            } else if (data[in] == '=') {
                // As Python used to write it, wrongly
                result.append('=');
                ++in;
            } else if (in + 1 < length && isASCIIHexDigit(data[in]) && isASCIIHexDigit(data[in + 1])) {
                result.append(static_cast<uint8_t>(toASCIIHexValue(data[in], data[in + 1])));
                in += 2;
            } else
                result.append('=');
        } else if (*isHeader && data[in] == '_') {
            result.append(' ');
            ++in;
        } else
            result.append(data[in++]);
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(bytesOrMemoryError(globalObject, result)));
}

// b2a_qp(data, quotetabs=False, istext=True, header=False). CPython goes through it twice, the first time to find how long it will be.
PYTHON_NATIVE(binasciiB2AQP)
{
    NATIVE_PROLOGUE();
    Bytes data;
    if (!toBuffer(globalObject, args[0], data))
        return { };
    auto quotesTabs = toBoolArgument(globalObject, args.at(1), false);
    RETURN_IF_EXCEPTION(scope, { });
    auto isText = toBoolArgument(globalObject, args.at(2), true);
    RETURN_IF_EXCEPTION(scope, { });
    auto isHeader = toBoolArgument(globalObject, args.at(3), false);
    RETURN_IF_EXCEPTION(scope, { });
    constexpr unsigned maxLineSize = 76;
    size_t length = data.size();
    // Whichever way the first line ends, they all will.
    size_t firstNewline = data.find('\n');
    bool endsLinesWithCRLF = firstNewline != notFound && firstNewline && data[firstNewline - 1] == '\r';

    Vector<uint8_t, 128> out;
    bool hasRoom = true;
    auto append = [&] (uint8_t c) { hasRoom = hasRoom && out.tryConstructAndAppend(c); };
    auto appendHex = [&] (uint8_t c) {
        append(upperNibbleToASCIIHexDigit(c));
        append(lowerNibbleToASCIIHexDigit(c));
    };
    auto appendNewline = [&] {
        if (endsLinesWithCRLF)
            append('\r');
        append('\n');
    };
    unsigned lineLength = 0;
    for (size_t in = 0; in < length && hasRoom;) {
        uint8_t c = data[in];
        bool isLast = in + 1 == length;
        bool isEncoded = c > 126 || c == '=' || (*isHeader && c == '_')
            || (c == '.' && !lineLength && (isLast || data[in + 1] == '\n' || data[in + 1] == '\r' || !data[in + 1]))
            || (!*isText && (c == '\r' || c == '\n'))
            || ((c == '\t' || c == ' ') && isLast)
            || (c < 33 && c != '\r' && c != '\n' && (*quotesTabs || (c != '\t' && c != ' ')));
        if (isEncoded) {
            if (lineLength + 3 >= maxLineSize) {
                append('=');
                appendNewline();
                lineLength = 0;
            }
            append('=');
            appendHex(c);
            ++in;
            lineLength += 3;
            continue;
        }
        if (*isText && (c == '\n' || (!isLast && c == '\r' && data[in + 1] == '\n'))) {
            lineLength = 0;
            // A space or a tab at the end of a line would be lost.
            if (!out.isEmpty() && (out.last() == ' ' || out.last() == '\t')) {
                uint8_t blank = out.last();
                out.last() = '=';
                appendHex(blank);
            }
            appendNewline();
            in += c == '\r' ? 2 : 1;
            continue;
        }
        if (!isLast && data[in + 1] != '\n' && lineLength + 1 >= maxLineSize) {
            append('=');
            appendNewline();
            lineLength = 0;
        }
        ++lineLength;
        append(*isHeader && c == ' ' ? '_' : c);
        ++in;
    }
    if (!hasRoom)
        return JSValue::encode(raiseMemoryError(globalObject, scope));
    RELEASE_AND_RETURN(scope, JSValue::encode(newBytes(globalObject, out.span())));
}

JSObject* createBinasciiModule(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRealm* realm = globalObject->pyRealm();
    auto& state = realm->moduleState<BinasciiModuleState>();
    if (!state.error) {
        // PyErr_NewException()
        auto newException = [&] (ASCIILiteral name, BuiltinType base) -> PyType* {
            PyDict* contents = PyDict::create(globalObject);
            contents->setString(globalObject, "__module__"_s, jsNontrivialString(vm, "binascii"_s));
            JSValue type = newType(globalObject, realm->typeType(), jsNontrivialString(vm, name), PyTuple::create(globalObject, { realm->type(base) }), contents, nullptr);
            RETURN_IF_EXCEPTION(scope, nullptr);
            return asType(type);
        };
        PyType* error = newException("Error"_s, BuiltinType::ValueError);
        RETURN_IF_EXCEPTION(scope, nullptr);
        PyType* incomplete = newException("Incomplete"_s, BuiltinType::Exception);
        RETURN_IF_EXCEPTION(scope, nullptr);
        state.error.set(vm, realm, error);
        state.incomplete.set(vm, realm, incomplete);
    }
    JSObject* module = newBuiltinModule(globalObject, "binascii"_s);
    addFunction(globalObject, module, "a2b_uu"_s, binasciiA2BUU);
    addFunction(globalObject, module, "b2a_uu"_s, binasciiB2AUU);
    addFunction(globalObject, module, "a2b_base64"_s, binasciiA2BBase64);
    addFunction(globalObject, module, "b2a_base64"_s, binasciiB2ABase64);
    addFunction(globalObject, module, "a2b_hex"_s, binasciiA2BHex);
    addFunction(globalObject, module, "b2a_hex"_s, binasciiB2AHex);
    addFunction(globalObject, module, "hexlify"_s, binasciiB2AHex);
    addFunction(globalObject, module, "unhexlify"_s, binasciiA2BHex);
    addFunction(globalObject, module, "crc_hqx"_s, binasciiCRCHQX);
    addFunction(globalObject, module, "crc32"_s, binasciiCRC32);
    addFunction(globalObject, module, "a2b_qp"_s, binasciiA2BQP);
    addFunction(globalObject, module, "b2a_qp"_s, binasciiB2AQP);
    module->putDirect(vm, Identifier::fromString(vm, "Error"_s), state.error.get());
    module->putDirect(vm, Identifier::fromString(vm, "Incomplete"_s), state.incomplete.get());
    return module;
}

} } // namespace JSC::Python
