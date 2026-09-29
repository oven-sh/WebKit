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

/* ------------------------------------------------------------------
   The code in this module was based on a download from:
      http://www.math.sci.hiroshima-u.ac.jp/~m-mat/MT/MT2002/emt19937ar.html

   It was modified in 2002 by Raymond Hettinger as follows:

    * the principal computational lines untouched.

    * renamed genrand_res53() to random_random() and wrapped
      in python calling/return code.

    * genrand_uint32() and the helper functions, init_genrand()
      and init_by_array(), were declared static, wrapped in
      Python calling/return code.  also, their global data
      references were replaced with structure references.

    * unused functions from the original were deleted.
      new, original C python code was added to implement the
      Random() interface.

   The following are the verbatim comments from the original code:

   A C-program for MT19937, with initialization improved 2002/1/26.
   Coded by Takuji Nishimura and Makoto Matsumoto.

   Before using, initialize the state by using init_genrand(seed)
   or init_by_array(init_key, key_length).

   Copyright (C) 1997 - 2002, Makoto Matsumoto and Takuji Nishimura,
   All rights reserved.

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:

     1. Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.

     2. Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.

     3. The names of its contributors may not be used to endorse or promote
    products derived from this software without specific prior written
    permission.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
   CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.


   Any feedback is very welcome.
   http://www.math.sci.hiroshima-u.ac.jp/~m-mat/MT/emt.html
   email: m-mat @ math.sci.hiroshima-u.ac.jp (remove space)
*/

#include "config.h"
#include "PythonBuiltins.h"

#include "JSCInlines.h"
#include "PyRealm.h"
#include "PyStateObject.h"
#include "PyTuple.h"
#include "PythonNumbers.h"
#include "PythonOperations.h"
#include "PythonTime.h"
#include <unistd.h>
#include <wtf/CryptographicallyRandomNumber.h>

// The module _random: Modules/_randommodule.c of CPython, which the above is from.

namespace JSC { namespace Python {

namespace {

/* Period parameters -- These are all magic.  Don't change. */
constexpr int N = 624;
constexpr int M = 397;
constexpr uint32_t MATRIX_A = 0x9908b0dfU; /* constant vector a */
constexpr uint32_t UPPER_MASK = 0x80000000U; /* most significant w-r bits */
constexpr uint32_t LOWER_MASK = 0x7fffffffU; /* least significant r bits */

struct RandomState final : NativeState {
    PYTHON_NATIVE_STATE(RandomState);
    int index { 0 };
    std::array<uint32_t, N> state { };
};

template<typename Visitor> void RandomState::visit(Visitor&) { }

struct RandomModuleState final : NativeState {
    PYTHON_NATIVE_STATE(RandomModuleState);
    WriteBarrier<PyType> random;
};

template<typename Visitor> void RandomModuleState::visit(Visitor& visitor) { visitor.append(random); }

/* generates a random number on [0,0xffffffff]-interval */
uint32_t genrand_uint32(RandomState& self)
{
    uint32_t y;
    static constexpr uint32_t mag01[2] = { 0x0U, MATRIX_A };
    /* mag01[x] = x * MATRIX_A  for x=0,1 */
    auto& mt = self.state;
    if (self.index >= N) { /* generate N words at one time */
        int kk;
        for (kk = 0; kk < N - M; kk++) {
            y = (mt[kk] & UPPER_MASK) | (mt[kk + 1] & LOWER_MASK);
            mt[kk] = mt[kk + M] ^ (y >> 1) ^ mag01[y & 0x1U];
        }
        for (; kk < N - 1; kk++) {
            y = (mt[kk] & UPPER_MASK) | (mt[kk + 1] & LOWER_MASK);
            mt[kk] = mt[kk + (M - N)] ^ (y >> 1) ^ mag01[y & 0x1U];
        }
        y = (mt[N - 1] & UPPER_MASK) | (mt[0] & LOWER_MASK);
        mt[N - 1] = mt[M - 1] ^ (y >> 1) ^ mag01[y & 0x1U];
        self.index = 0;
    }
    y = mt[self.index++];
    y ^= (y >> 11);
    y ^= (y << 7) & 0x9d2c5680U;
    y ^= (y << 15) & 0xefc60000U;
    y ^= (y >> 18);
    return y;
}

/* initializes mt[N] with a seed */
void init_genrand(RandomState& self, uint32_t s)
{
    int mti;
    auto& mt = self.state;
    mt[0] = s;
    for (mti = 1; mti < N; mti++) {
        mt[mti] = (1812433253U * (mt[mti - 1] ^ (mt[mti - 1] >> 30)) + mti);
        /* See Knuth TAOCP Vol2. 3rd Ed. P.106 for multiplier. */
        /* In the previous versions, MSBs of the seed affect   */
        /* only MSBs of the array mt[].                                */
        /* 2002/01/09 modified by Makoto Matsumoto                     */
    }
    self.index = mti;
}

/* initialize by an array with array-length */
/* init_key is the array for initializing keys */
void init_by_array(RandomState& self, std::span<const uint32_t> init_key)
{
    size_t key_length = init_key.size();
    size_t i, j, k; /* was signed in the original code. RDH 12/16/2002 */
    auto& mt = self.state;
    init_genrand(self, 19650218U);
    i = 1;
    j = 0;
    k = (static_cast<size_t>(N) > key_length ? N : key_length);
    for (; k; k--) {
        mt[i] = (mt[i] ^ ((mt[i - 1] ^ (mt[i - 1] >> 30)) * 1664525U)) + init_key[j] + static_cast<uint32_t>(j); /* non linear */
        i++;
        j++;
        if (i >= N) {
            mt[0] = mt[N - 1];
            i = 1;
        }
        if (j >= key_length)
            j = 0;
    }
    for (k = N - 1; k; k--) {
        mt[i] = (mt[i] ^ ((mt[i - 1] ^ (mt[i - 1] >> 30)) * 1566083941U)) - static_cast<uint32_t>(i); /* non linear */
        i++;
        if (i >= N) {
            mt[0] = mt[N - 1];
            i = 1;
        }
    }
    mt[0] = 0x80000000U; /* MSB is 1; assuring non-zero initial array */
}

/*
 * The rest is Python-specific code, neither part of, nor derived from, the
 * Twister download.
 */

// random_seed(). `argument` may be empty.
void seed(JSGlobalObject* globalObject, RandomState& self, JSValue argument)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (!argument || isNone(argument)) {
        // random_seed_urandom(). There is no failing to be had of it here, and so no falling back on what time it is.
        std::array<uint32_t, N> key;
        cryptographicallyRandomValues(asMutableByteSpan(std::span(key)));
        init_by_array(self, key);
        return;
    }
    // An int is taken without its sign, whatever its class may say abs() of it is. Anything else is taken for what it hashes to.
    Vector<uint64_t, 4> digits;
    if (isInstance(globalObject, argument, globalObject->pyRealm()->typeInt()))
        digits = digitsOfInt(classify(argument));
    else {
        int64_t hashed = hash(globalObject, argument);
        RETURN_IF_EXCEPTION(scope, void());
        if (hashed)
            digits.append(static_cast<uint64_t>(hashed));
    }
    // In pieces of 32 bits, from the right, and no more of them than it takes
    Vector<uint32_t, 8> key;
    for (uint64_t digit : digits) {
        key.append(static_cast<uint32_t>(digit));
        key.append(static_cast<uint32_t>(digit >> 32));
    }
    while (key.size() > 1 && !key.last())
        key.removeLast();
    if (key.isEmpty())
        key.append(0);
    init_by_array(self, key.span());
}

} // anonymous namespace

// PyType_GenericNew(): whatever it is given is for __init__().
PYTHON_NATIVE(randomNew)
{
    return JSValue::encode(PyStateObject::create(globalObject->vm(), asType(callFrame->uncheckedArgument(0))->instanceStructure(), makeUnique<RandomState>()));
}

// Random([seed])
PYTHON_NATIVE(randomInit)
{
    NATIVE_PROLOGUE();
    // A class that is derived from it and has a __new__() of its own may take what it likes by name.
    PyType* base = realm->moduleState<RandomModuleState>().random.get();
    PyType* type = typeOf(globalObject, args[0]);
    if ((type == base || type->lookup(vm, names.dunder_new) == base->lookup(vm, names.dunder_new)) && !args.checkNoKeywords(globalObject, scope, "Random"_s))
        return { };
    if (args.size() > 2)
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("Random expected at most 1 argument, got "_s, args.size() - 1)));
    seed(globalObject, stateOf<RandomState>(args[0]), args.at(1));
    RETURN_IF_EXCEPTION(scope, { });
    RETURN_NONE();
}

// random(): x in the interval [0, 1), with 53 bits to it
PYTHON_NATIVE(randomRandom)
{
    auto& self = stateOf<RandomState>(callFrame->uncheckedArgument(0));
    UNUSED_PARAM(globalObject);
    uint32_t a = genrand_uint32(self) >> 5;
    uint32_t b = genrand_uint32(self) >> 6;
    return JSValue::encode(floatFromDouble((a * 67108864.0 + b) * (1.0 / 9007199254740992.0)));
}

// seed(n=None, /)
PYTHON_NATIVE(randomSeed)
{
    NATIVE_PROLOGUE();
    seed(globalObject, stateOf<RandomState>(args[0]), args.at(1));
    RETURN_IF_EXCEPTION(scope, { });
    RETURN_NONE();
}

PYTHON_NATIVE(randomGetState)
{
    NATIVE_PROLOGUE();
    auto& self = stateOf<RandomState>(args[0]);
    PyTuple* state = PyTuple::create(globalObject, N + 1);
    for (int i = 0; i < N; ++i)
        state->initializeAt(vm, i, intFromInt64(globalObject, self.state[i]));
    state->initializeAt(vm, N, jsNumber(self.index));
    return JSValue::encode(state);
}

// setstate(state, /)
PYTHON_NATIVE(randomSetState)
{
    NATIVE_PROLOGUE();
    auto& self = stateOf<RandomState>(args[0]);
    if (!isTuple(args[1]))
        return JSValue::encode(raiseTypeError(globalObject, scope, "state vector must be a tuple"_s));
    PyTuple* state = asTuple(args[1]);
    if (state->length() != N + 1)
        return JSValue::encode(raiseValueError(globalObject, scope, "state vector is the wrong size"_s));
    std::array<uint32_t, N> newState;
    for (int i = 0; i < N; ++i) {
        // PyLong_AsUnsignedLong()
        JSValue element = state->at(i);
        if (!isInstance(globalObject, element, realm->typeInt()))
            return JSValue::encode(raiseTypeError(globalObject, scope, "an integer is required"_s));
        Number number = classify(element);
        if (number.kind == Number::Kind::Small ? number.small < 0 : number.big->sign())
            return JSValue::encode(raise(globalObject, scope, BuiltinType::OverflowError, "can't convert negative value to unsigned int"_s));
        auto digits = digitsOfInt(number);
        if (digits.size() > 1)
            return JSValue::encode(raise(globalObject, scope, BuiltinType::OverflowError, "Python int too large to convert to C unsigned long"_s));
        newState[i] = digits.isEmpty() ? 0 : static_cast<uint32_t>(digits[0]);
    }
    auto index = toCLong(globalObject, state->at(N));
    RETURN_IF_EXCEPTION(scope, { });
    if (*index < 0 || *index > N)
        return JSValue::encode(raiseValueError(globalObject, scope, "invalid state"_s));
    self.index = static_cast<int>(*index);
    self.state = newState;
    RETURN_NONE();
}

// getrandbits(k, /)
PYTHON_NATIVE(randomGetRandBits)
{
    NATIVE_PROLOGUE();
    auto& self = stateOf<RandomState>(args[0]);
    // _PyLong_UInt64_Converter()
    JSValue integer = toInt(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    if (compareInts(integer, jsNumber(0)) < 0)
        return JSValue::encode(raiseValueError(globalObject, scope, "Cannot convert negative int"_s));
    uint64_t k = lowBitsOfInt(integer);
    if (compareInts(integer, intFromUInt64(globalObject, k)))
        return JSValue::encode(raise(globalObject, scope, BuiltinType::OverflowError, "Python int too large for C uint64_t"_s));
    if (!k)
        return JSValue::encode(jsNumber(0));
    if (k <= 32)
        return JSValue::encode(intFromInt64(globalObject, genrand_uint32(self) >> (32 - k)));
    uint64_t words = (k - 1u) / 32u + 1u;
    Vector<uint64_t, 4> digits;
    if (words > std::numeric_limits<int64_t>::max() / 4u || !digits.tryGrow((words + 1) / 2))
        return JSValue::encode(raiseMemoryError(globalObject, scope));
    digits.fill(0);
    // 32 bits at a time, from the least
    for (uint64_t i = 0; i < words; ++i, k -= 32) {
        uint32_t r = genrand_uint32(self);
        if (k < 32)
            r >>= (32 - k); // It is the low bits that are let go.
        digits[i / 2] |= static_cast<uint64_t>(r) << (i % 2 * 32);
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(intFromDigits(globalObject, digits.span())));
}

JSObject* createRandomModule(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    auto& state = realm->moduleState<RandomModuleState>();
    using Kind = PyNativeFunction::Kind;
    constexpr auto notChecked = PyNativeFunction::Arguments::AreNotChecked;
    if (!state.random) {
        PyType* type = createBuiltinType(globalObject, "_random.Random"_s, realm->typeObject(), PyType::Layout::Native, PyType::IsBaseType);
        type->setInstanceStructure(vm, PyStateObject::createStructure(vm, globalObject, type));
        state.random.set(vm, realm, type);
        addMethods(globalObject, type, {
            { "__new__"_s, randomNew, Kind::New, 0, "($type, /, *args, **kwargs)"_s, notChecked },
            { "__init__"_s, randomInit, Kind::Wrapper, 0, { }, notChecked },
            { "random"_s, randomRandom },
            { "seed"_s, randomSeed },
            { "getstate"_s, randomGetState },
            { "setstate"_s, randomSetState },
            { "getrandbits"_s, randomGetRandBits },
        });
    }
    JSObject* module = newBuiltinModule(globalObject, "_random"_s);
    module->putDirect(vm, Identifier::fromString(vm, "Random"_s), state.random.get());
    return module;
}

} } // namespace JSC::Python
