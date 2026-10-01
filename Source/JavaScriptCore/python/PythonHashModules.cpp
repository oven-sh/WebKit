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
#include "PyRealm.h"
#include "PyStateObject.h"
#include "PyType.h"
#include "PythonBytes.h"
#include "PythonHashAlgorithms.h"
#include "PythonHashlib.h"
#include "PythonOperations.h"
#include <wtf/Variant.h>

// The modules _md5, _sha1, _sha2, _sha3 and _blake2: md5module.c, sha1module.c, sha2module.c, sha3module.c and blake2module.c of CPython's Modules. They are what hashlib has to go by where there is no _hashlib, which is over a
// library that a host may have and the engine has not. The five are as alike as they look, so they are here together.

namespace JSC { namespace Python {

Buffer bufferToHash(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (stringIn(value)) {
        raiseTypeError(globalObject, scope, "Strings must be encoded before hashing"_s);
        return { };
    }
    bool has = hasBuffer(globalObject, value);
    RETURN_IF_EXCEPTION(scope, { });
    if (!has) {
        raiseTypeError(globalObject, scope, "object supporting the buffer API required"_s);
        return { };
    }
    RELEASE_AND_RETURN(scope, tryBufferOf(globalObject, value));
}

JSValue dataToHash(JSGlobalObject* globalObject, JSValue data, JSValue string)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (data && string) {
        raiseTypeError(globalObject, scope, "'data' and 'string' are mutually exclusive and support for 'string' keyword parameter is slated for removal in a future version."_s);
        return { };
    }
    return data ? data : string;
}

String hexOf(std::span<const uint8_t> bytes)
{
    std::span<Latin1Character> characters;
    String result = String::createUninitialized(bytes.size() * 2, characters);
    for (size_t i = 0; i < bytes.size(); ++i) {
        characters[2 * i] = lowerNibbleToLowercaseASCIIHexDigit(bytes[i] >> 4);
        characters[2 * i + 1] = lowerNibbleToLowercaseASCIIHexDigit(bytes[i]);
    }
    return result;
}

namespace {

enum class Algorithm : uint8_t {
    MD5,
    SHA1,
    SHA224,
    SHA256,
    SHA384,
    SHA512,
    SHA3_224,
    SHA3_256,
    SHA3_384,
    SHA3_512,
    SHAKE128,
    SHAKE256,
    Blake2b,
    Blake2s,
};
constexpr unsigned numberOfAlgorithms = static_cast<unsigned>(Algorithm::Blake2s) + 1;

struct AlgorithmDescription {
    ASCIILiteral typeName; // With its module
    ASCIILiteral name; // What an object says that it is
};

constexpr AlgorithmDescription descriptions[numberOfAlgorithms] = {
    { "_md5.md5"_s, "md5"_s },
    { "_sha1.sha1"_s, "sha1"_s },
    { "_sha2.SHA224Type"_s, "sha224"_s },
    { "_sha2.SHA256Type"_s, "sha256"_s },
    { "_sha2.SHA384Type"_s, "sha384"_s },
    { "_sha2.SHA512Type"_s, "sha512"_s },
    { "_sha3.sha3_224"_s, "sha3_224"_s },
    { "_sha3.sha3_256"_s, "sha3_256"_s },
    { "_sha3.sha3_384"_s, "sha3_384"_s },
    { "_sha3.sha3_512"_s, "sha3_512"_s },
    { "_sha3.shake_128"_s, "shake_128"_s },
    { "_sha3.shake_256"_s, "shake_256"_s },
    { "_blake2.blake2b"_s, "blake2b"_s },
    { "_blake2.blake2s"_s, "blake2s"_s },
};

bool isSHA3(Algorithm algorithm) { return algorithm >= Algorithm::SHA3_224 && algorithm <= Algorithm::SHAKE256; }
bool isSHAKE(Algorithm algorithm) { return algorithm == Algorithm::SHAKE128 || algorithm == Algorithm::SHAKE256; }

struct HashState final : NativeState {
    PYTHON_NATIVE_STATE(HashState);
    HashState(Algorithm algorithm, AnyHash&& hash)
        : algorithm(algorithm)
        , hash(WTF::move(hash))
    {
    }

    void update(std::span<const uint8_t> data) { updateHash(hash, data); }

    // 0 for a SHAKE, of which there is as much as is asked for.
    size_t digestSize() const
    {
        return WTF::switchOn(hash,
            [] (const MD5Hash&) -> size_t { return MD5Hash::digestSize; },
            [] (const SHA1Hash&) -> size_t { return SHA1Hash::digestSize; },
            [&] (const KeccakHash& hash) -> size_t { return isSHAKE(algorithm) ? 0 : (200 - hash.rate()) / 2; },
            [] (const auto& hash) -> size_t { return hash.digestSize(); });
    }

    size_t blockSize() const
    {
        return WTF::switchOn(hash,
            [] (const KeccakHash& hash) -> size_t { return hash.rate(); },
            [] (const auto& hash) -> size_t { return std::remove_cvref_t<decltype(hash)>::blockSize; });
    }

    void digest(std::span<uint8_t> out) const { digestOfHash(hash, out); }

    Algorithm algorithm;
    AnyHash hash;
};

template<typename Visitor> void HashState::visit(Visitor&) { }

struct HashModulesState final : NativeState {
    PYTHON_NATIVE_STATE(HashModulesState);
    WriteBarrier<PyType> types[numberOfAlgorithms];
};

template<typename Visitor> void HashModulesState::visit(Visitor& visitor)
{
    for (auto& type : types)
        visitor.append(type);
}

AnyHash makeHash(Algorithm algorithm)
{
    switch (algorithm) {
    case Algorithm::MD5:
        return MD5Hash();
    case Algorithm::SHA1:
        return SHA1Hash();
    case Algorithm::SHA224:
        return SHA256Hash(28);
    case Algorithm::SHA256:
        return SHA256Hash(32);
    case Algorithm::SHA384:
        return SHA512Hash(48);
    case Algorithm::SHA512:
        return SHA512Hash(64);
    case Algorithm::SHA3_224:
        return KeccakHash(448, 6);
    case Algorithm::SHA3_256:
        return KeccakHash(512, 6);
    case Algorithm::SHA3_384:
        return KeccakHash(768, 6);
    case Algorithm::SHA3_512:
        return KeccakHash(1024, 6);
    case Algorithm::SHAKE128:
        return KeccakHash(256, 0x1f);
    case Algorithm::SHAKE256:
        return KeccakHash(512, 0x1f);
    case Algorithm::Blake2b:
    case Algorithm::Blake2s:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

JSValue newHashObject(JSGlobalObject* globalObject, PyType* type, Algorithm algorithm, AnyHash&& hash, JSValue data)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* object = PyStateObject::create(vm, type->instanceStructure(), makeUnique<HashState>(algorithm, WTF::move(hash)));
    if (data) {
        Buffer buffer = bufferToHash(globalObject, data);
        RETURN_IF_EXCEPTION(scope, { });
        object->state<HashState>().update(buffer);
    }
    return object;
}

PyType* typeFor(JSGlobalObject* globalObject, Algorithm algorithm)
{
    return globalObject->pyRealm()->moduleState<HashModulesState>().types[static_cast<unsigned>(algorithm)].get();
}

} // anonymous namespace

// md5(data=b'', *, usedforsecurity=True, string=None), and the like of _sha1 and _sha2, which are functions. The classes of _sha3 are called the same way.
PYTHON_NATIVE(hashNew)
{
    auto algorithm = unpack<Algorithm>(callFrame, 0);
    NATIVE_PROLOGUE();
    // The class comes first if it is a class that has been called.
    unsigned first = isSHA3(algorithm) ? 1 : 0;
    if (JSValue usedForSecurity = args.at(first + 1)) {
        isTrue(globalObject, usedForSecurity);
        RETURN_IF_EXCEPTION(scope, { });
    }
    JSValue data = dataToHash(globalObject, args.at(first), args.at(first + 2));
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(newHashObject(globalObject, first ? asType(args[0]) : typeFor(globalObject, algorithm), algorithm, makeHash(algorithm), data)));
}

// py_blake2b_or_s_new()
PYTHON_NATIVE(blake2New)
{
    auto algorithm = unpack<Algorithm>(callFrame, 0);
    NATIVE_PROLOGUE();
    bool isB = algorithm == Algorithm::Blake2b;
    int maxDigestSize = isB ? Blake2bHash::maxDigestSize : Blake2sHash::maxDigestSize;
    size_t maxKeySize = isB ? Blake2bHash::maxKeySize : Blake2sHash::maxKeySize;
    size_t saltSize = isB ? Blake2bHash::saltSize : Blake2sHash::saltSize;
    size_t personSize = isB ? Blake2bHash::personSize : Blake2sHash::personSize;

    // In the order that they are in.
    enum { Type, Data, DigestSize, Key, Salt, Person, Fanout, Depth, LeafSize, NodeOffset, NodeDepth, InnerSize, LastNode, UsedForSecurity, StringArgument };
    auto toInt = [&] (unsigned index, int otherwise) -> int {
        JSValue given = args.at(index);
        if (!given)
            return otherwise;
        auto value = toCInt(globalObject, given);
        return value ? *value : 0;
    };
    int digestSize = toInt(DigestSize, maxDigestSize);
    RETURN_IF_EXCEPTION(scope, { });
    Buffer key, salt, person;
    for (auto [index, buffer] : { std::pair { unsigned { Key }, &key }, std::pair { unsigned { Salt }, &salt }, std::pair { unsigned { Person }, &person } }) {
        if (JSValue given = args.at(index)) {
            *buffer = bufferOf(globalObject, given);
            RETURN_IF_EXCEPTION(scope, { });
        }
    }
    int fanout = toInt(Fanout, 1);
    RETURN_IF_EXCEPTION(scope, { });
    int depth = toInt(Depth, 1);
    RETURN_IF_EXCEPTION(scope, { });
    unsigned long leafSize = 0;
    if (JSValue given = args.at(LeafSize)) {
        auto value = toUnsigned<unsigned long>(globalObject, given, "unsigned long"_s);
        RETURN_IF_EXCEPTION(scope, { });
        leafSize = *value;
    }
    unsigned long long nodeOffset = 0;
    if (JSValue given = args.at(NodeOffset)) {
        auto value = toUnsigned<unsigned long long>(globalObject, given, "unsigned long long"_s);
        RETURN_IF_EXCEPTION(scope, { });
        nodeOffset = *value;
    }
    int nodeDepth = toInt(NodeDepth, 0);
    RETURN_IF_EXCEPTION(scope, { });
    int innerSize = toInt(InnerSize, 0);
    RETURN_IF_EXCEPTION(scope, { });
    bool isLastNode = false;
    if (JSValue given = args.at(LastNode)) {
        isLastNode = isTrue(globalObject, given);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (JSValue given = args.at(UsedForSecurity)) {
        isTrue(globalObject, given);
        RETURN_IF_EXCEPTION(scope, { });
    }
    JSValue data = dataToHash(globalObject, args.at(Data), args.at(StringArgument));
    RETURN_IF_EXCEPTION(scope, { });

    auto valueError = [&] (const String& message) { return JSValue::encode(raise(globalObject, scope, BuiltinType::ValueError, message)); };
    if (digestSize <= 0 || digestSize > maxDigestSize)
        return valueError(concatenate("digest_size for "_s, isB ? "Blake2b"_s : "Blake2s"_s, " must be between 1 and "_s, maxDigestSize, " bytes, here it is "_s, digestSize));
    if (salt.size() > saltSize)
        return valueError(concatenate("maximum salt length is "_s, saltSize, " bytes"_s));
    if (person.size() > personSize)
        return valueError(concatenate("maximum person length is "_s, personSize, " bytes"_s));
    if (fanout < 0 || fanout > 255)
        return valueError("fanout must be between 0 and 255"_s);
    if (depth <= 0 || depth > 255)
        return valueError("depth must be between 1 and 255"_s);
    if (leafSize > 0xFFFFFFFFu)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::OverflowError, "leaf_size is too large"_s));
    if (!isB && nodeOffset > 0xFFFFFFFFFFFFull)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::OverflowError, "node_offset is too large"_s));
    if (nodeDepth < 0 || nodeDepth > 255)
        return valueError("node_depth must be between 0 and 255"_s);
    if (innerSize < 0 || innerSize > maxDigestSize)
        return valueError(concatenate("inner_size must be between 0 and is "_s, maxDigestSize));
    if (key.size() > maxKeySize)
        return valueError(concatenate("maximum key length is "_s, maxKeySize, " bytes"_s));

    Blake2Parameters parameters {
        static_cast<uint8_t>(digestSize), key.span(), salt.span(), person.span(), static_cast<uint8_t>(fanout), static_cast<uint8_t>(depth), static_cast<uint32_t>(leafSize), nodeOffset,
        static_cast<uint8_t>(nodeDepth), static_cast<uint8_t>(innerSize), isLastNode
    };
    AnyHash hash = isB ? AnyHash(Blake2bHash(parameters)) : AnyHash(Blake2sHash(parameters));
    RELEASE_AND_RETURN(scope, JSValue::encode(newHashObject(globalObject, asType(args[0]), algorithm, WTF::move(hash), data)));
}

// copy()
PYTHON_NATIVE(hashCopy)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    auto& state = stateOf<HashState>(args[0]);
    return JSValue::encode(PyStateObject::create(vm, args[0].asCell()->structure(), makeUnique<HashState>(state.algorithm, AnyHash(state.hash))));
}

// update(obj, /)
PYTHON_NATIVE(hashUpdate)
{
    NATIVE_PROLOGUE();
    Buffer buffer = bufferToHash(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    stateOf<HashState>(args[0]).update(buffer);
    RETURN_NONE();
}

// digest() and hexdigest(), and those of a SHAKE, which are told how much: _SHAKE_digest()
PYTHON_NATIVE(hashDigest)
{
    bool isHex = unpack<bool>(callFrame, 0);
    NATIVE_PROLOGUE();
    auto& state = stateOf<HashState>(args[0]);
    size_t length = state.digestSize();
    if (isSHAKE(state.algorithm)) {
        auto given = toUnsigned<unsigned long>(globalObject, args[1], "unsigned long"_s);
        RETURN_IF_EXCEPTION(scope, { });
        if (*given >= (1 << 29))
            return JSValue::encode(raise(globalObject, scope, BuiltinType::ValueError, "length is too large"_s));
        length = *given;
    }
    Vector<uint8_t, 64> digest;
    if (!digest.tryGrow(length)) [[unlikely]]
        return JSValue::encode(raiseMemoryError(globalObject, scope));
    state.digest(digest.mutableSpan());
    if (isHex)
        return JSValue::encode(jsString(vm, hexOf(digest.span())));
    RELEASE_AND_RETURN(scope, JSValue::encode(newBytes(globalObject, digest.span())));
}

static PyType* ensureType(JSGlobalObject* globalObject, Algorithm algorithm)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    auto& slot = realm->moduleState<HashModulesState>().types[static_cast<unsigned>(algorithm)];
    if (slot)
        return slot.get();
    using Kind = PyNativeFunction::Kind;
    PyType* type = createBuiltinType(globalObject, descriptions[static_cast<unsigned>(algorithm)].typeName, realm->typeObject(), PyType::Layout::Native, 0);
    type->setInstanceStructure(vm, PyStateObject::createStructure(vm, globalObject, type));
    slot.set(vm, realm, type);
    // Those of _md5, _sha1 and _sha2 are made by the functions of the module, and by nothing else.
    constexpr auto ofTheClass = PyNativeFunction::Arguments::AreThoseOfTheClass;
    if (isSHA3(algorithm)) {
        // The six have the one py_sha3_new(), which says that it is the first of them.
        addMethods(globalObject, type, { { "__new__"_s, hashNew, Kind::New, pack(algorithm), "sha3_224($type, /, data=b'', *, usedforsecurity=True, string=None)"_s, ofTheClass } });
    } else if (algorithm >= Algorithm::Blake2b) {
        addMethods(globalObject, type, { { "__new__"_s, blake2New, Kind::New, pack(algorithm),
            "($type, /, data=b'', *, digest_size=0, key=b'', salt=b'', person=b'', fanout=1, depth=1, leaf_size=0, node_offset=0, node_depth=0, inner_size=0, last_node=False, usedforsecurity=True, string=None)"_s, ofTheClass } });
    }
    bool copyTakesDefiningClass = algorithm <= Algorithm::SHA512;
    addMethods(globalObject, type, {
        { "copy"_s, hashCopy, Kind::Method, 0, { }, copyTakesDefiningClass ? PyNativeFunction::Arguments::AreCheckedAsWithDefiningClass : PyNativeFunction::Arguments::AreChecked },
        { "digest"_s, hashDigest, Kind::Method, pack(false) },
        { "hexdigest"_s, hashDigest, Kind::Method, pack(true) },
        { "update"_s, hashUpdate },
    });
    addGetSet(globalObject, type, "block_size"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return jsNumber(static_cast<int32_t>(stateOf<HashState>(self).blockSize())); });
    addGetSet(globalObject, type, "name"_s, [] (JSGlobalObject* globalObject, JSValue self) -> JSValue { return jsNontrivialString(globalObject->vm(), descriptions[static_cast<unsigned>(stateOf<HashState>(self).algorithm)].name); });
    addGetSet(globalObject, type, "digest_size"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return jsNumber(static_cast<int32_t>(stateOf<HashState>(self).digestSize())); });
    if (isSHA3(algorithm)) {
        addGetSet(globalObject, type, "_capacity_bits"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return jsNumber(static_cast<int32_t>(1600 - stateOf<HashState>(self).blockSize() * 8)); });
        addGetSet(globalObject, type, "_rate_bits"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return jsNumber(static_cast<int32_t>(stateOf<HashState>(self).blockSize() * 8)); });
        addGetSet(globalObject, type, "_suffix"_s, [] (JSGlobalObject* globalObject, JSValue self) -> JSValue {
            uint8_t suffix = std::get<KeccakHash>(stateOf<HashState>(self).hash).suffix();
            return newBytes(globalObject, std::span { &suffix, 1 });
        });
    }
    if (algorithm >= Algorithm::Blake2b) {
        bool isB = algorithm == Algorithm::Blake2b;
        type->putDirect(vm, Identifier::fromString(vm, "SALT_SIZE"_s), jsNumber(static_cast<int32_t>(isB ? Blake2bHash::saltSize : Blake2sHash::saltSize)));
        type->putDirect(vm, Identifier::fromString(vm, "PERSON_SIZE"_s), jsNumber(static_cast<int32_t>(isB ? Blake2bHash::personSize : Blake2sHash::personSize)));
        type->putDirect(vm, Identifier::fromString(vm, "MAX_KEY_SIZE"_s), jsNumber(static_cast<int32_t>(isB ? Blake2bHash::maxKeySize : Blake2sHash::maxKeySize)));
        type->putDirect(vm, Identifier::fromString(vm, "MAX_DIGEST_SIZE"_s), jsNumber(static_cast<int32_t>(isB ? Blake2bHash::maxDigestSize : Blake2sHash::maxDigestSize)));
    }
    return type;
}

static void addMinimumSize(JSGlobalObject* globalObject, JSObject* module)
{
    VM& vm = globalObject->vm();
    module->putDirect(vm, Identifier::fromString(vm, "_GIL_MINSIZE"_s), jsNumber(minimumSizeToHashWithoutTheGIL));
}

JSObject* createMD5Module(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    JSObject* module = newBuiltinModule(globalObject, "_md5"_s);
    addFunction(globalObject, module, "md5"_s, hashNew, pack(Algorithm::MD5));
    module->putDirect(vm, Identifier::fromString(vm, "MD5Type"_s), ensureType(globalObject, Algorithm::MD5));
    addMinimumSize(globalObject, module);
    return module;
}

JSObject* createSHA1Module(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    JSObject* module = newBuiltinModule(globalObject, "_sha1"_s);
    addFunction(globalObject, module, "sha1"_s, hashNew, pack(Algorithm::SHA1));
    module->putDirect(vm, Identifier::fromString(vm, "SHA1Type"_s), ensureType(globalObject, Algorithm::SHA1));
    addMinimumSize(globalObject, module);
    return module;
}

JSObject* createSHA2Module(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    JSObject* module = newBuiltinModule(globalObject, "_sha2"_s);
    addFunction(globalObject, module, "sha224"_s, hashNew, pack(Algorithm::SHA224));
    addFunction(globalObject, module, "sha256"_s, hashNew, pack(Algorithm::SHA256));
    addFunction(globalObject, module, "sha384"_s, hashNew, pack(Algorithm::SHA384));
    addFunction(globalObject, module, "sha512"_s, hashNew, pack(Algorithm::SHA512));
    module->putDirect(vm, Identifier::fromString(vm, "SHA224Type"_s), ensureType(globalObject, Algorithm::SHA224));
    module->putDirect(vm, Identifier::fromString(vm, "SHA256Type"_s), ensureType(globalObject, Algorithm::SHA256));
    module->putDirect(vm, Identifier::fromString(vm, "SHA384Type"_s), ensureType(globalObject, Algorithm::SHA384));
    module->putDirect(vm, Identifier::fromString(vm, "SHA512Type"_s), ensureType(globalObject, Algorithm::SHA512));
    addMinimumSize(globalObject, module);
    return module;
}

JSObject* createSHA3Module(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    JSObject* module = newBuiltinModule(globalObject, "_sha3"_s);
    for (auto algorithm : { Algorithm::SHA3_224, Algorithm::SHA3_256, Algorithm::SHA3_384, Algorithm::SHA3_512, Algorithm::SHAKE128, Algorithm::SHAKE256 })
        module->putDirect(vm, Identifier::fromString(vm, descriptions[static_cast<unsigned>(algorithm)].name), ensureType(globalObject, algorithm));
    // Whose it is. It is not HACL's here, but that is what a program that asks is looking for: not the one before it, which was slower.
    module->putDirect(vm, Identifier::fromString(vm, "implementation"_s), jsNontrivialString(vm, "HACL"_s));
    addMinimumSize(globalObject, module);
    return module;
}

JSObject* createBlake2Module(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    JSObject* module = newBuiltinModule(globalObject, "_blake2"_s);
    module->putDirect(vm, Identifier::fromString(vm, "blake2b"_s), ensureType(globalObject, Algorithm::Blake2b));
    module->putDirect(vm, Identifier::fromString(vm, "blake2s"_s), ensureType(globalObject, Algorithm::Blake2s));
    auto add = [&] (ASCIILiteral name, size_t value) { module->putDirect(vm, Identifier::fromString(vm, name), jsNumber(static_cast<int32_t>(value))); };
    add("BLAKE2B_SALT_SIZE"_s, Blake2bHash::saltSize);
    add("BLAKE2B_PERSON_SIZE"_s, Blake2bHash::personSize);
    add("BLAKE2B_MAX_KEY_SIZE"_s, Blake2bHash::maxKeySize);
    add("BLAKE2B_MAX_DIGEST_SIZE"_s, Blake2bHash::maxDigestSize);
    add("BLAKE2S_SALT_SIZE"_s, Blake2sHash::saltSize);
    add("BLAKE2S_PERSON_SIZE"_s, Blake2sHash::personSize);
    add("BLAKE2S_MAX_KEY_SIZE"_s, Blake2sHash::maxKeySize);
    add("BLAKE2S_MAX_DIGEST_SIZE"_s, Blake2sHash::maxDigestSize);
    addMinimumSize(globalObject, module);
    return module;
}

} } // namespace JSC::Python
