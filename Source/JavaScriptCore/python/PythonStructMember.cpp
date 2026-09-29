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
#include "PythonStructMember.h"

#include "JSCInlines.h"
#include "PyRealm.h"
#include "PythonOperations.h"

namespace JSC { namespace Python {

static bool checkIsNotDeleted(JSGlobalObject* globalObject, ThrowScope& scope, JSValue value)
{
    if (value)
        return true;
    raiseTypeError(globalObject, scope, "can't delete numeric/char attribute"_s);
    return false;
}

// What is stored is stored whether or not the warning is made an error of.
template<typename T>
static bool setFromLong(JSGlobalObject* globalObject, JSValue value, T& field, ASCIILiteral warning)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (!checkIsNotDeleted(globalObject, scope, value))
        return false;
    auto number = toCLong(globalObject, value);
    RETURN_IF_EXCEPTION(scope, false);
    field = static_cast<T>(*number);
    if (*number > std::numeric_limits<T>::max() || *number < std::numeric_limits<T>::min())
        RELEASE_AND_RETURN(scope, warn(globalObject, BuiltinType::RuntimeWarning, warning));
    return true;
}

bool setMember(JSGlobalObject* globalObject, JSValue value, short& field) { return setFromLong(globalObject, value, field, "Truncation of value to short"_s); }
bool setMember(JSGlobalObject* globalObject, JSValue value, unsigned short& field) { return setFromLong(globalObject, value, field, "Truncation of value to unsigned short"_s); }
bool setMember(JSGlobalObject* globalObject, JSValue value, int& field) { return setFromLong(globalObject, value, field, "Truncation of value to int"_s); }

// One that is less than nothing has always been let pass.
template<typename T>
static bool setUnsigned(JSGlobalObject* globalObject, JSValue value, T& field, ASCIILiteral tooLarge)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (!checkIsNotDeleted(globalObject, scope, value))
        return false;
    JSValue integer = toInt(globalObject, value);
    RETURN_IF_EXCEPTION(scope, false);
    if (compareInts(integer, jsNumber(0)) < 0) {
        auto number = toCLong(globalObject, integer);
        RETURN_IF_EXCEPTION(scope, false);
        field = static_cast<T>(static_cast<unsigned long long>(*number));
        RELEASE_AND_RETURN(scope, warn(globalObject, BuiltinType::RuntimeWarning, "Writing negative value into unsigned field"_s));
    }
    // It has 64 bits or fewer if it is what its low 64 bits are.
    uint64_t number = lowBitsOfInt(integer);
    if (compareInts(integer, intFromUInt64(globalObject, number))) {
        raise(globalObject, scope, BuiltinType::OverflowError, tooLarge);
        return false;
    }
    field = static_cast<T>(number);
    if (number > std::numeric_limits<T>::max())
        RELEASE_AND_RETURN(scope, warn(globalObject, BuiltinType::RuntimeWarning, "Truncation of value to unsigned int"_s));
    return true;
}

bool setMember(JSGlobalObject* globalObject, JSValue value, unsigned& field) { return setUnsigned(globalObject, value, field, "Python int too large to convert to C unsigned long"_s); }
bool setMember(JSGlobalObject* globalObject, JSValue value, unsigned long long& field) { return setUnsigned(globalObject, value, field, "int too big to convert"_s); }

bool setMember(JSGlobalObject* globalObject, JSValue value, long long& field)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (!checkIsNotDeleted(globalObject, scope, value))
        return false;
    auto number = toCLongLong(globalObject, value);
    RETURN_IF_EXCEPTION(scope, false);
    field = *number;
    return true;
}

} } // namespace JSC::Python
