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
#include "PythonHashlib.h"

#include "JSCInlines.h"
#include "PyObjects.h"
#include "PyRealm.h"
#include "PyStateObject.h"
#include "PythonBuiltins.h"
#include "PythonCodecs.h"
#include "PythonHashAlgorithms.h"
#include "PythonIO.h"
#include "PythonOperations.h"
#include "PythonText.h"

// _hmac: Modules/hmacmodule.c of CPython, which is HMAC (RFC 2104) over the hashes that CPython has of its own. Those are in PythonHashAlgorithms.h here.

namespace JSC { namespace Python {

namespace {

// py_hmac_hinfo: a hash that there can be an HMAC of
struct HMACHashInfo {
    ASCIILiteral name; // As HACL* has it
    ASCIILiteral hashlibName; // As hashlib has it, which is what is shown. Null if that is the same.
    uint32_t blockSize;
    uint32_t digestSize;

    ASCIILiteral displayName() const { return hashlibName.isNull() ? name : hashlibName; }
};

// py_hmac_static_hinfo
constexpr HMACHashInfo hmacHashes[] = {
    { "md5"_s, { }, 64, 16 },
    { "sha1"_s, { }, 64, 20 },
    { "sha2_224"_s, "sha224"_s, 64, 28 },
    { "sha2_256"_s, "sha256"_s, 64, 32 },
    { "sha2_384"_s, "sha384"_s, 128, 48 },
    { "sha2_512"_s, "sha512"_s, 128, 64 },
    { "sha3_224"_s, { }, 144, 28 },
    { "sha3_256"_s, { }, 136, 32 },
    { "sha3_384"_s, { }, 104, 48 },
    { "sha3_512"_s, { }, 72, 64 },
    { "blake2s_32"_s, "blake2s"_s, 64, 32 },
    { "blake2b_32"_s, "blake2b"_s, 128, 64 },
};
constexpr size_t maximumHMACBlockSize = 144; // Py_hmac_hash_max_block_size
constexpr size_t maximumHMACDigestSize = 64; // Py_hmac_hash_max_digest_size

AnyHash newHash(unsigned index)
{
    const HMACHashInfo& info = hmacHashes[index];
    switch (index) {
    case 0:
        return MD5Hash();
    case 1:
        return SHA1Hash();
    case 2:
    case 3:
        return SHA256Hash(info.digestSize);
    case 4:
    case 5:
        return SHA512Hash(info.digestSize);
    case 6:
    case 7:
    case 8:
    case 9:
        return KeccakHash(info.digestSize * 16, 6);
    case 10:
        return Blake2sHash(Blake2Parameters { static_cast<uint8_t>(info.digestSize), { }, { }, { } });
    default:
        return Blake2bHash(Blake2Parameters { static_cast<uint8_t>(info.digestSize), { }, { }, { } });
    }
}

// H((K ^ opad) || H((K ^ ipad) || message)). What there is of each hash once it has been given its pad is kept, and the inner one is given the message as that comes.
class HMACComputation {
public:
    HMACComputation(unsigned index, std::span<const uint8_t> key)
        : m_index(index)
        , m_inner(newHash(index))
        , m_outer(newHash(index))
    {
        const HMACHashInfo& info = hmacHashes[index];
        std::array<uint8_t, maximumHMACBlockSize> block { };
        // A key that is longer than a block is hashed first.
        if (key.size() > info.blockSize) {
            AnyHash ofKey = newHash(index);
            updateHash(ofKey, key);
            digestOfHash(ofKey, std::span { block }.first(info.digestSize));
        } else
            memcpySpan(std::span { block }.first(key.size()), key);
        std::array<uint8_t, maximumHMACBlockSize> pad;
        for (size_t i = 0; i < info.blockSize; ++i)
            pad[i] = block[i] ^ 0x36;
        updateHash(m_inner, std::span { pad }.first(info.blockSize));
        for (size_t i = 0; i < info.blockSize; ++i)
            pad[i] = block[i] ^ 0x5c;
        updateHash(m_outer, std::span { pad }.first(info.blockSize));
    }

    const HMACHashInfo& info() const { return hmacHashes[m_index]; }
    void update(std::span<const uint8_t> data) { updateHash(m_inner, data); }

    // It is left as it was.
    void digest(std::span<uint8_t> out) const
    {
        std::array<uint8_t, maximumHMACDigestSize> inner;
        digestOfHash(m_inner, std::span { inner }.first(info().digestSize));
        AnyHash outer = m_outer;
        updateHash(outer, std::span { inner }.first(info().digestSize));
        digestOfHash(outer, out);
    }

private:
    unsigned m_index;
    AnyHash m_inner;
    AnyHash m_outer;
};

// HMACObject
struct HMACState final : NativeState {
    PYTHON_NATIVE_STATE(HMACState);
    explicit HMACState(const HMACComputation& computation)
        : computation(computation)
    {
    }
    HMACComputation computation;
};

template<typename Visitor> void HMACState::visit(Visitor&) { }

// hmacmodule_state
struct HMACModuleState final : NativeState {
    PYTHON_NATIVE_STATE(HMACModuleState);
    WriteBarrier<PyType> unknownHashError;
    WriteBarrier<PyType> type;
};

template<typename Visitor>
void HMACModuleState::visit(Visitor& visitor)
{
    visitor.append(unknownHashError);
    visitor.append(type);
}

HMACModuleState& hmacModuleState(JSGlobalObject* globalObject) { return globalObject->pyRealm()->moduleState<HMACModuleState>(); }

// find_hash_info_by_utf8name(), which is given a string of C's: it ends at the first NUL.
std::optional<unsigned> findHashByName(const String& given)
{
    StringView name = StringView(given).left(given.find('\0'));
    for (unsigned i = 0; i < std::size(hmacHashes); ++i) {
        if (name == hmacHashes[i].name || (!hmacHashes[i].hashlibName.isNull() && name == hmacHashes[i].hashlibName))
            return i;
    }
    return std::nullopt;
}

// find_hash_info(): by a name, as it is or in small letters. Nothing else is taken for a hash. Nothing if it raised.
std::optional<unsigned> findHash(JSGlobalObject* globalObject, JSValue reference)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (JSString* string = stringIn(reference)) {
        // PyUnicode_AsUTF8()
        encodeString(globalObject, string, "utf-8"_s, "strict"_s);
        RETURN_IF_EXCEPTION(scope, std::nullopt);
        String name = string->value(globalObject);
        RETURN_IF_EXCEPTION(scope, std::nullopt);
        if (auto found = findHashByName(name))
            return found;
        JSValue lower = callMethodNamed(globalObject, reference, Identifier::fromString(vm, "lower"_s));
        RETURN_IF_EXCEPTION(scope, std::nullopt);
        JSString* lowerString = stringIn(lower);
        if (!lowerString) {
            raiseTypeError(globalObject, scope, "bad argument type for built-in operation"_s);
            return std::nullopt;
        }
        encodeString(globalObject, lowerString, "utf-8"_s, "strict"_s);
        RETURN_IF_EXCEPTION(scope, std::nullopt);
        name = lowerString->value(globalObject);
        RETURN_IF_EXCEPTION(scope, std::nullopt);
        if (auto found = findHashByName(name))
            return found;
    }
    String shown = repr(globalObject, reference);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    raise(globalObject, scope, hmacModuleState(globalObject).unknownHashError.get(), concatenate("unsupported hash type: "_s, shown));
    return std::nullopt;
}

JSValue raiseTooLong(JSGlobalObject* globalObject, ThrowScope& scope, ASCIILiteral what)
{
    return raise(globalObject, scope, BuiltinType::OverflowError, concatenate(what, " length exceeds "_s, UINT32_MAX));
}

// _hmac_new_impl()
PYTHON_NATIVE(hmacNew)
{
    NATIVE_PROLOGUE();
    JSValue reference = args.at(2);
    if (!reference)
        return JSValue::encode(raiseTypeError(globalObject, scope, "new() missing 1 required argument 'digestmod'"_s));
    auto index = findHash(globalObject, reference);
    RETURN_IF_EXCEPTION(scope, { });
    std::unique_ptr<HMACState> state;
    {
        Buffer key = bufferToHash(globalObject, args[0]);
        RETURN_IF_EXCEPTION(scope, { });
        if (key.span().size() > UINT32_MAX)
            return JSValue::encode(raiseTooLong(globalObject, scope, "key"_s));
        state = makeUnique<HMACState>(HMACComputation(*index, key.span()));
    }
    if (JSValue message = args.at(1); message && !isNone(message)) {
        Buffer data = bufferToHash(globalObject, message);
        RETURN_IF_EXCEPTION(scope, { });
        state->computation.update(data.span());
    }
    return JSValue::encode(PyStateObject::create(vm, hmacModuleState(globalObject).type->instanceStructure(), WTF::move(state)));
}

// _hmac_HMAC_copy_impl()
PYTHON_NATIVE(hmacCopy)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    return JSValue::encode(PyStateObject::create(vm, hmacModuleState(globalObject).type->instanceStructure(), makeUnique<HMACState>(stateOf<HMACState>(args[0]).computation)));
}

// _hmac_HMAC_update_impl()
PYTHON_NATIVE(hmacUpdate)
{
    NATIVE_PROLOGUE();
    Buffer data = bufferToHash(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    stateOf<HMACState>(args[0]).computation.update(data.span());
    RETURN_NONE();
}

// _hmac_HMAC_digest_impl() and _hmac_HMAC_hexdigest_impl()
PYTHON_NATIVE(hmacDigest)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    auto& computation = stateOf<HMACState>(args[0]).computation;
    std::array<uint8_t, maximumHMACDigestSize> digest;
    auto out = std::span { digest }.first(computation.info().digestSize);
    computation.digest(out);
    if (unpack<bool>(callFrame, 0))
        return JSValue::encode(jsString(vm, hexOf(out)));
    return JSValue::encode(newBytes(globalObject, out));
}

// HMACObject_repr()
PYTHON_NATIVE(hmacRepr)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    return JSValue::encode(jsString(vm, concatenate('<', stateOf<HMACState>(args[0]).computation.info().displayName(), " HMAC object @ "_s, addressOf(args[0].asCell()), '>')));
}

// Py_HMAC_HACL_ONESHOT()
JSValue computeAtOnce(JSGlobalObject* globalObject, unsigned index, JSValue keyObject, JSValue messageObject)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    Buffer key = bufferToHash(globalObject, keyObject);
    RETURN_IF_EXCEPTION(scope, { });
    if (key.span().size() > UINT32_MAX)
        return raiseTooLong(globalObject, scope, "key"_s);
    Buffer message = bufferToHash(globalObject, messageObject);
    RETURN_IF_EXCEPTION(scope, { });
    if (message.span().size() > UINT32_MAX)
        return raiseTooLong(globalObject, scope, "message"_s);
    HMACComputation computation(index, key.span());
    computation.update(message.span());
    std::array<uint8_t, maximumHMACDigestSize> digest;
    auto out = std::span { digest }.first(hmacHashes[index].digestSize);
    computation.digest(out);
    return newBytes(globalObject, out);
}

// _hmac_compute_digest_impl()
PYTHON_NATIVE(hmacComputeDigest)
{
    NATIVE_PROLOGUE();
    auto index = findHash(globalObject, args[2]);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(computeAtOnce(globalObject, *index, args[0], args[1])));
}

// _hmac_compute_md5_impl() and the rest
PYTHON_NATIVE(hmacComputeOne)
{
    NativeArguments args(callFrame);
    return JSValue::encode(computeAtOnce(globalObject, unpack<unsigned>(callFrame, 0), args[0], args[1]));
}

} // namespace

JSObject* createHMACModule(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    using Kind = PyNativeFunction::Kind;
    auto& state = hmacModuleState(globalObject);

    if (!state.type) {
        PyType* error = newException(globalObject, "_hmac"_s, "UnknownHashError"_s, realm->typeValueError());
        RETURN_IF_EXCEPTION(scope, nullptr);
        state.unknownHashError.set(vm, realm, error);

        PyType* type = createBuiltinType(globalObject, "_hmac.HMAC"_s, realm->typeObject(), PyType::Layout::Native, 0);
        type->setInstanceStructure(vm, PyStateObject::createStructure(vm, globalObject, type));
        state.type.set(vm, realm, type);
        addMethods(globalObject, type, {
            { "__repr__"_s, hmacRepr },
            { "copy"_s, hmacCopy, Kind::Method, 0, { }, PyNativeFunction::Arguments::AreCheckedAsWithDefiningClass },
            { "update"_s, hmacUpdate },
            { "digest"_s, hmacDigest, Kind::Method, pack(false) },
            { "hexdigest"_s, hmacDigest, Kind::Method, pack(true) },
        });
        addGetSet(globalObject, type, "name"_s, [] (JSGlobalObject* globalObject, JSValue self) -> JSValue { return jsString(globalObject->vm(), concatenate("hmac-"_s, stateOf<HMACState>(self).computation.info().displayName())); });
        addGetSet(globalObject, type, "block_size"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return jsNumber(static_cast<int32_t>(stateOf<HMACState>(self).computation.info().blockSize)); });
        addGetSet(globalObject, type, "digest_size"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return jsNumber(static_cast<int32_t>(stateOf<HMACState>(self).computation.info().digestSize)); });
    }

    JSObject* module = newBuiltinModule(globalObject, "_hmac"_s);
    addFunction(globalObject, module, "new"_s, hmacNew);
    addFunction(globalObject, module, "compute_digest"_s, hmacComputeDigest);
    constexpr ASCIILiteral names[] = { "compute_md5"_s, "compute_sha1"_s, "compute_sha224"_s, "compute_sha256"_s, "compute_sha384"_s, "compute_sha512"_s, "compute_sha3_224"_s, "compute_sha3_256"_s, "compute_sha3_384"_s, "compute_sha3_512"_s, "compute_blake2s_32"_s, "compute_blake2b_32"_s };
    static_assert(std::size(names) == std::size(hmacHashes));
    for (unsigned i = 0; i < std::size(names); ++i)
        addFunction(globalObject, module, names[i], hmacComputeOne, pack(i));
    module->putDirect(vm, Identifier::fromString(vm, "UnknownHashError"_s), state.unknownHashError->object());
    module->putDirect(vm, Identifier::fromString(vm, "HMAC"_s), state.type->object());
    module->putDirect(vm, Identifier::fromString(vm, "_GIL_MINSIZE"_s), jsNumber(minimumSizeToHashWithoutTheGIL));
    return module;
}

} } // namespace JSC::Python
