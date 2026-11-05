/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "PyAbstract.h"
#include "PyBytes.h"
#include "PyDict.h"
#include "PyErrors.h"
#include "PyInternals.h"
#include "PyList.h"
#include "PySet.h"
#include "PyTuple.h"

namespace JSC {
namespace Python {

// getItem(), setItem() and contains() for the slow paths of the interpreter, which have them in their own bodies: the cases for the
// built-in containers, and a call for the rest.

ALWAYS_INLINE JSValue getItemInline(JSGlobalObject* globalObject, JSValue object, JSValue key)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (object.isCell()) {
        JSCell* cell = object.asCell();
        switch (cell->type()) {
        case ArrayType:
            if (key.isInt32()) {
                auto* list = uncheckedDowncast<JSArray>(cell);
                int64_t index = key.asInt32();
                int64_t size = list->length();
                if (index < 0)
                    index += size;
                if (index >= 0 && index < size) [[likely]]
                    return listGet(list, static_cast<unsigned>(index));
                return raiseIndexError(globalObject, scope, "list index out of range"_s);
            }
            RELEASE_AND_RETURN(scope, listGetItem(globalObject, object, key));
        case PyTupleType: {
            auto* tuple = uncheckedDowncast<PyTuple>(cell);
            if (tuple->type() != rt.tupleType())
                break;
            if (key.isInt32()) {
                int64_t index = key.asInt32();
                int64_t size = tuple->length();
                if (index < 0)
                    index += size;
                if (index >= 0 && index < size) [[likely]]
                    return tuple->at(static_cast<unsigned>(index));
                return raiseIndexError(globalObject, scope, "tuple index out of range"_s);
            }
            RELEASE_AND_RETURN(scope, tupleGetItem(globalObject, object, key));
        }
        case StringType:
            RELEASE_AND_RETURN(scope, strGetItem(globalObject, object, key));
        case PyDictType: {
            auto* dict = uncheckedDowncast<PyDict>(cell);
            if (dict->type() != rt.dictType())
                break;
            JSValue value = dict->get(globalObject, key);
            if (value) [[likely]] {
                scope.release();
                return value;
            }
            RETURN_IF_EXCEPTION(scope, { });
            return raiseKeyError(globalObject, scope, key);
        }
        case PyBytesType: {
            auto* bytes = uncheckedDowncast<PyBytes>(cell);
            if (bytes->type() != rt.bytesType())
                break;
            if (key.isInt32()) {
                int64_t index = key.asInt32() < 0 ? key.asInt32() + static_cast<int64_t>(bytes->length()) : key.asInt32();
                if (static_cast<uint64_t>(index) < bytes->length()) [[likely]]
                    return jsNumber(bytes->data()[index]);
            }
            RELEASE_AND_RETURN(scope, bytesGetItem(globalObject, object, key));
        }
        case PyByteArrayType: {
            auto* array = uncheckedDowncast<PyByteArray>(cell);
            if (array->type() != rt.bytearrayType())
                break;
            if (key.isInt32()) {
                int64_t index = key.asInt32() < 0 ? key.asInt32() + static_cast<int64_t>(array->length()) : key.asInt32();
                if (static_cast<uint64_t>(index) < array->length()) [[likely]]
                    return jsNumber(array->span()[index]);
            }
            RELEASE_AND_RETURN(scope, bytesGetItem(globalObject, object, key));
        }
        default:
            break;
        }
    }

    RELEASE_AND_RETURN(scope, getItemByMethod(globalObject, object, key));
}

ALWAYS_INLINE void setItemInline(JSGlobalObject* globalObject, JSValue object, JSValue key, JSValue value)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (object.isCell()) {
        JSCell* cell = object.asCell();
        if (cell->type() == PyDictType) {
            auto* dict = uncheckedDowncast<PyDict>(cell);
            if (dict->type() == rt.dictType())
                RELEASE_AND_RETURN(scope, dict->set(globalObject, key, value));
        } else if (cell->type() == ArrayType && key.isInt32()) {
            auto* list = uncheckedDowncast<JSArray>(cell);
            int64_t index = key.asInt32();
            int64_t size = list->length();
            if (index < 0)
                index += size;
            if (index >= 0 && index < size) [[likely]]
                RELEASE_AND_RETURN(scope, listSet(globalObject, list, static_cast<unsigned>(index), value));
            raiseIndexError(globalObject, scope, "list assignment index out of range"_s);
            return;
        } else if (cell->type() == PyByteArrayType && key.isInt32() && value.isInt32()) {
            auto* array = uncheckedDowncast<PyByteArray>(cell);
            int64_t index = key.asInt32() < 0 ? key.asInt32() + static_cast<int64_t>(array->length()) : key.asInt32();
            if (array->type() == rt.bytearrayType() && static_cast<uint64_t>(index) < array->length() && static_cast<uint32_t>(value.asInt32()) < 256) [[likely]] {
                array->mutableSpan()[index] = static_cast<uint8_t>(value.asInt32());
                return;
            }
        }
    }

    RELEASE_AND_RETURN(scope, setItemByMethod(globalObject, object, key, value));
}

ALWAYS_INLINE bool containsInline(JSGlobalObject* globalObject, JSValue container, JSValue item)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (container.isCell()) {
        JSCell* cell = container.asCell();
        switch (cell->type()) {
        case PyDictType:
            if (auto* dict = uncheckedDowncast<PyDict>(cell); dict->type() == rt.dictType())
                RELEASE_AND_RETURN(scope, dict->contains(globalObject, item));
            break;
        case PySetType:
            if (auto* set = uncheckedDowncast<PySet>(cell); set->type() == rt.setType() || set->type() == rt.frozensetType())
                RELEASE_AND_RETURN(scope, setContains(globalObject, set, item));
            break;
        case ArrayType:
            RELEASE_AND_RETURN(scope, listContains(globalObject, uncheckedDowncast<JSArray>(cell), item));
        case PyTupleType:
            if (auto* tuple = uncheckedDowncast<PyTuple>(cell); tuple->type() == rt.tupleType())
                RELEASE_AND_RETURN(scope, tupleContains(globalObject, tuple, item));
            break;
        case StringType:
            RELEASE_AND_RETURN(scope, strContains(globalObject, uncheckedDowncast<JSString>(cell), item));
        default:
            break;
        }
    }

    RELEASE_AND_RETURN(scope, containsByMethod(globalObject, container, item));
}

} // namespace Python
} // namespace JSC
