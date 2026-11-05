/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyModule.h"

#include "JSCInlines.h"
#include "PyAbstract.h"
#include "PyBytes.h"
#include "PyDict.h"
#include "PyErrors.h"
#include "PyInternals.h"
#include "PyNumbers.h"
#include "PyStr.h"
#include "PyThreads.h"
#include "PyTuple.h"
#include <wtf/MonotonicTime.h>
#include <wtf/OSRandomSource.h>
#include <wtf/ProcessID.h>
#include <wtf/WallTime.h>

#if OS(LINUX)
#include <errno.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>
#ifndef GRND_NONBLOCK
#define GRND_NONBLOCK 0x0001
#endif
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// ---- Random bytes of the system ----

bool systemRandomBytes(JSGlobalObject* globalObject, std::span<uint8_t> buffer, SystemRandomBytes purpose)
{
    if (buffer.empty())
        return true;
#if OS(LINUX)
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool isForSeed = purpose == SystemRandomBytes::ForSeed;
    // Handlers run between the pieces of what a program has asked for. False: one of them raised.
    auto handleSignals = [&] {
        return isForSeed || !failed(scope, checkSignals(globalObject));
    };

    while (!buffer.empty()) {
        ssize_t count;
        {
            // Not getrandom(), which is newer than the oldest C library this may run with.
            AllowThreads allow(globalObject);
            count = static_cast<ssize_t>(syscall(SYS_getrandom, buffer.data(), buffer.size(), isForSeed ? GRND_NONBLOCK : 0u));
        }
        if (count < 0 && errno != EINTR) {
            // The kernel is too old for it, or a filter forbids it: the device knows none of that. Nor does it ever wait, and a
            // seed takes what it can get.
            if (errno == ENOSYS || errno == EPERM || isForSeed)
                break;
            raiseOSError(globalObject, scope, errno);
            return false;
        }
        // A signal makes it give less than it was asked for, or nothing.
        bool isAll = count == static_cast<ssize_t>(buffer.size());
        if (count > 0)
            buffer = buffer.subspan(static_cast<size_t>(count));
        if (!isAll && !handleSignals())
            return false;
    }
    if (buffer.empty())
        return true;

    static constexpr ASCIILiteral device = "/dev/urandom"_s;
    int descriptor;
    int errorNumber = 0;
    do {
        {
            AllowThreads allow(globalObject);
            descriptor = open(device.characters(), O_RDONLY | O_CLOEXEC);
        }
        errorNumber = descriptor < 0 ? errno : 0;
        if (errorNumber == EINTR && !handleSignals())
            return false;
    } while (errorNumber == EINTR);
    if (errorNumber) {
        if (isForSeed)
            return false;
        switch (errorNumber) {
        case EACCES:
        case ENODEV:
        case ENOENT:
        case ENXIO:
            raiseNotImplementedError(globalObject, scope, device, " (or equivalent) not found"_s);
            return false;
        default:
            raiseOSError(globalObject, scope, errorNumber, makeStr(vm, device));
            return false;
        }
    }
    bool didRaise = false;
    while (!buffer.empty()) {
        ssize_t count;
        {
            AllowThreads allow(globalObject);
            count = read(descriptor, buffer.data(), buffer.size());
        }
        if (count > 0) {
            buffer = buffer.subspan(static_cast<size_t>(count));
            continue;
        }
        errorNumber = count ? errno : EIO;
        if (errorNumber != EINTR)
            break;
        errorNumber = 0;
        if (!handleSignals()) {
            didRaise = true;
            break;
        }
    }
    close(descriptor);
    if (didRaise)
        return false;
    if (!errorNumber)
        return true;
    if (!isForSeed)
        raiseOSError(globalObject, scope, errorNumber, makeStr(vm, device));
    return false;
#else
    UNUSED_PARAM(purpose);
    AllowThreads allow(globalObject);
    WTF::cryptographicallyRandomValuesFromOS(buffer);
    return true;
#endif
}

// The module _random: the Mersenne Twister MT19937 of Matsumoto and Nishimura. (Modules/_randommodule.c in CPython.)
// A seed gives the numbers it gives in CPython.

static constexpr unsigned randomWordCount = 624;
static constexpr unsigned randomOffset = 397;
static constexpr uint64_t randomMaxBits = 1 << 30; // No int has more: JSBigInt::maxLengthBits.

// A generator is a record of one field, the words of the state in a PyByteStorage; `number` is the index of the next word.
enum : unsigned { RandomWords, RandomFieldCount };

extern const TypeDef randomTypeDef;

static ALWAYS_INLINE uint32_t* randomWordsOf(PyRecord* generator)
{
    return reinterpret_cast<uint32_t*>(uncheckedDowncast<PyByteStorage>(generator->get(RandomWords).asCell())->data());
}

static uint32_t randomNextWord(PyRecord* generator)
{
    uint32_t* words = randomWordsOf(generator);
    if (generator->number >= randomWordCount) {
        for (unsigned i = 0; i < randomWordCount; ++i) {
            uint32_t y = (words[i] & 0x80000000u) | (words[(i + 1) % randomWordCount] & 0x7fffffffu);
            words[i] = words[(i + randomOffset) % randomWordCount] ^ (y >> 1) ^ (y & 1 ? 0x9908b0dfu : 0);
        }
        generator->number = 0;
    }
    uint32_t y = words[generator->number++];
    y ^= y >> 11;
    y ^= (y << 7) & 0x9d2c5680u;
    y ^= (y << 15) & 0xefc60000u;
    y ^= y >> 18;
    return y;
}

// init_by_array() of the reference implementation.
static void randomSeedWithKey(PyRecord* generator, std::span<const uint32_t> key)
{
    uint32_t* words = randomWordsOf(generator);
    words[0] = 19650218u;
    for (unsigned i = 1; i < randomWordCount; ++i)
        words[i] = 1812433253u * (words[i - 1] ^ (words[i - 1] >> 30)) + i;
    generator->number = randomWordCount;

    auto mixed = [&](unsigned i, uint32_t multiplier) { return words[i] ^ ((words[i - 1] ^ (words[i - 1] >> 30)) * multiplier); };
    auto advance = [&](unsigned& i) {
        if (++i >= randomWordCount) {
            words[0] = words[randomWordCount - 1];
            i = 1;
        }
    };
    unsigned i = 1;
    size_t j = 0;
    for (size_t k = std::max<size_t>(randomWordCount, key.size()); k; --k) {
        words[i] = mixed(i, 1664525u) + key[j] + static_cast<uint32_t>(j);
        advance(i);
        if (++j >= key.size())
            j = 0;
    }
    for (unsigned k = randomWordCount - 1; k; --k) {
        words[i] = mixed(i, 1566083941u) - i;
        advance(i);
    }
    words[0] = 0x80000000u;
}

// False: an exception.
static bool randomSeedWith(JSGlobalObject* globalObject, PyRecord* generator, JSValue seed)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!seed || isNone(seed)) {
        uint32_t key[randomWordCount];
        bool hasKey = systemRandomBytes(globalObject, std::span<uint8_t> { reinterpret_cast<uint8_t*>(key), sizeof(key) }, SystemRandomBytes::ForSeed);
        scope.assertNoException();
        if (hasKey) {
            randomSeedWithKey(generator, key);
            return true;
        }
        // "If randomness sources are provided by the operating system, they are used instead of the system time."
        uint64_t wallTime = static_cast<uint64_t>(WallTime::now().secondsSinceEpoch().nanoseconds());
        uint64_t monotonicTime = static_cast<uint64_t>(MonotonicTime::now().secondsSinceEpoch().nanoseconds());
        uint32_t timesAndProcess[] = { static_cast<uint32_t>(getCurrentProcessID()), static_cast<uint32_t>(wallTime), static_cast<uint32_t>(wallTime >> 32), static_cast<uint32_t>(monotonicTime), static_cast<uint32_t>(monotonicTime >> 32) };
        randomSeedWithKey(generator, timesAndProcess);
        return true;
    }

    // The absolute value of an int; of anything else, the hash, taken for unsigned.
    Vector<uint32_t, 8> key;
    auto appendWord = [&](uint64_t word) {
        key.append(static_cast<uint32_t>(word));
        key.append(static_cast<uint32_t>(word >> 32));
    };
    if (isInt(seed)) {
        // A BigInt of JS's may be small, and 0n has no digits.
        JSValue value = normalizeInt(intValue(seed));
        if (isSmallInt(value)) {
            int64_t number = smallIntValue(value);
            appendWord(number < 0 ? -number : number);
        } else {
            JSBigInt* bigInt = value.asHeapBigInt();
            static_assert(sizeof(JSBigInt::Digit) == sizeof(uint64_t));
            for (unsigned i = 0; i < bigInt->length(); ++i)
                appendWord(bigInt->digit(i));
        }
    } else {
        int64_t hashValue = hash(globalObject, seed);
        RETURN_IF_EXCEPTION(scope, false);
        appendWord(static_cast<uint64_t>(hashValue));
    }
    while (key.size() > 1 && !key.last())
        key.removeLast();
    randomSeedWithKey(generator, key.span());
    return true;
}

static JSValue randomNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs&)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    PyRecord* generator = PyRecord::create(globalObject, PyType::from(cls), RandomFieldCount);
    PyByteStorage* storage = PyByteStorage::create(vm, rt.structure<PyByteStorage>(), randomWordCount * sizeof(uint32_t));
    zeroSpan(std::span<uint8_t> { storage->data(), storage->capacity() });
    generator->set(vm, RandomWords, storage);
    return generator;
}

static JSValue randomInit(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    // A subclass with a __new__ of its own may have keyword arguments for it.
    PyType* baseType = runtime(globalObject).typeFor(randomTypeDef);
    if (typeOf(globalObject, self)->lookup(Special::New) == baseType->lookup(Special::New) && failed(scope, rejectKeywords(globalObject, "Random"_s, args)))
        return { };
    if (failed(scope, checkPositional(globalObject, "Random"_s, args, 0, 1)))
        return { };
    if (failed(scope, randomSeedWith(globalObject, PyRecord::from(self), args.get(0))))
        return { };
    return none();
}

static JSValue randomSeed(JSGlobalObject* globalObject, JSValue self, JSValue seed)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, randomSeedWith(globalObject, PyRecord::from(self), seed)))
        return { };
    return none();
}

// 53 random bits, from the upper 27 of one word and the upper 26 of the next.
static JSValue randomRandom(JSGlobalObject* globalObject, JSValue self)
{
    PyRecord* generator = PyRecord::from(self);
    uint32_t a = randomNextWord(generator) >> 5;
    uint32_t b = randomNextWord(generator) >> 6;
    return makeFloat(globalObject, (a * 67108864.0 + b) * (1.0 / 9007199254740992.0));
}

static JSValue randomGetRandBits(JSGlobalObject* globalObject, JSValue self, JSValue count)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* generator = PyRecord::from(self);
    JSValue index = toIndex(globalObject, count);
    RETURN_IF_EXCEPTION(scope, { });
    if (intSign(index) < 0)
        return raiseValueError(globalObject, scope, "Cannot convert negative int"_s);
    auto converted = tryUInt64(index);
    if (!converted)
        return raiseOverflowError(globalObject, scope, "Python int too large for C uint64_t"_s);
    uint64_t k = *converted;
    if (!k)
        return jsNumber(0);
    if (k <= 32)
        return jsNumber(randomNextWord(generator) >> (32 - k));
    if (k > randomMaxBits)
        return raiseMemoryError(globalObject, scope, String());

    // From the least significant 32 bits up; of the last word, the upper bits are used.
    Vector<uint64_t, 8> words;
    words.fill(0, (k + 63) / 64);
    for (size_t i = 0; k; ++i) {
        uint32_t word = randomNextWord(generator);
        if (k < 32)
            word >>= 32 - k;
        words[i / 2] |= static_cast<uint64_t>(word) << (i % 2 * 32);
        k -= std::min<uint64_t>(k, 32);
    }
    while (!words.isEmpty() && !words.last())
        words.removeLast();
    if (words.isEmpty())
        return jsNumber(0);
    JSBigInt* result = JSBigInt::createFromWords(globalObject, words.span(), false);
    RETURN_IF_EXCEPTION(scope, { });
    return normalizeInt(result);
}

static JSValue randomGetState(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    PyRecord* generator = PyRecord::from(self);
    PyTuple* state = PyTuple::create(globalObject, randomWordCount + 1);
    uint32_t* words = randomWordsOf(generator);
    for (unsigned i = 0; i < randomWordCount; ++i)
        state->initializeAt(vm, i, jsNumber(words[i]));
    state->initializeAt(vm, randomWordCount, jsNumber(static_cast<int32_t>(generator->number)));
    return state;
}

static JSValue randomSetState(JSGlobalObject* globalObject, JSValue self, JSValue stateValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyTuple* state = PyTuple::tryFrom(stateValue);
    if (!state)
        return raiseTypeError(globalObject, scope, "state vector must be a tuple"_s);
    if (state->length() != randomWordCount + 1)
        return raiseValueError(globalObject, scope, "state vector is the wrong size"_s);

    uint32_t words[randomWordCount];
    for (unsigned i = 0; i < randomWordCount; ++i) {
        JSValue item = state->at(i);
        if (!isInt(item))
            return raiseTypeError(globalObject, scope, "an integer is required"_s);
        item = intValue(item);
        if (intSign(item) < 0)
            return raiseOverflowError(globalObject, scope, "can't convert negative value to unsigned int"_s);
        auto word = tryUInt64(item);
        if (!word)
            return raiseOverflowError(globalObject, scope, "Python int too large to convert to C unsigned long"_s);
        words[i] = static_cast<uint32_t>(*word);
    }
    int64_t index = toInt64(globalObject, state->at(randomWordCount));
    RETURN_IF_EXCEPTION(scope, { });
    if (index < 0 || index > randomWordCount)
        return raiseValueError(globalObject, scope, "invalid state"_s);

    PyRecord* generator = PyRecord::from(self);
    generator->number = index;
    memcpySpan(std::span<uint32_t> { randomWordsOf(generator), randomWordCount }, std::span<const uint32_t> { words });
    return none();
}

static constexpr NativeDef randomMethods[] = {
    PY_NEW(randomNew, "($type, *args, **kwargs)"),
    PY_SLOT("__init__", randomInit, "($self, /, *args, **kwargs)"),
    PY_METHOD("random", randomRandom, "($self, /)"),
    PY_METHOD("seed", randomSeed, "($self, n=None, /)"),
    PY_METHOD("getstate", randomGetState, "($self, /)"),
    PY_METHOD("setstate", randomSetState, "($self, state, /)"),
    PY_METHOD("getrandbits", randomGetRandBits, "($self, k, /)"),
};

constexpr TypeDef randomTypeDef { "_random.Random"_s, { TypeFlag::Base, TypeFlag::Immutable, TypeFlag::OwnLayout }, randomMethods, { }, "Random() -> create a random number generator with its own internal state."_s };

void initializeRandomModule(JSGlobalObject* globalObject, PyModule* module)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    module->dict()->set(vm, rt.names.dDoc(), makeStr(vm, "Module implements the Mersenne Twister random number generator."_s));
    PyType* type = rt.typeFor(randomTypeDef);
    // In CPython the type is made at run time, and such a type has this in its dict, where its instances find it.
    type->dict()->set(vm, rt.names.dModule(), makeStr(vm, "_random"_s));
    type->didChange(vm);
    module->add(globalObject, "Random"_s, type);
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
