/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "CApiType.h"

#include "CApiInstance.h"
#include "JSCInlines.h"
#include "PyAbstract.h"
#include "PyCodecs.h"
#include "PyDict.h"
#include "PyInternals.h"
#include "PyStr.h"
#include "PyTuple.h"

#if PYTHON_HAS_CAPI

// PyType_*: types that extensions make.

namespace JSC {
namespace Python {
namespace CApi {

CAPI_FILE(Type)

const ClassInfo MadeByExtension::s_info = { "PyCApiMadeByExtension"_s, nullptr, nullptr, nullptr, CREATE_METHOD_TABLE(MadeByExtension) };

MadeByExtension* MadeByExtension::create(VM& vm, Structure* structure)
{
    auto* made = new (NotNull, allocateCell<MadeByExtension>(vm)) MadeByExtension(vm, structure);
    made->finishCreation(vm);
    return made;
}

MadeByExtension* madeByExtension(PyType* type)
{
    if (!type->has(TypeFlag::InstancesHaveDataOfC))
        return nullptr;
    for (; type; type = type->base()) {
        if (auto* made = MadeByExtension::of(type))
            return made;
    }
    return nullptr;
}

// What C keeps in an instance is laid out, and ended, by the type of the extension.
bool instancesAreAlike(PyType* a, PyType* b)
{
    return madeByExtension(a) == madeByExtension(b);
}

// ---- Making a type ----

// The slots that have to do with the end of an object. (The others come with the ways in which C is called.)
static bool isSlotOfLife(int slot)
{
    switch (slot) {
    case tp_clear:
    case tp_dealloc:
    case tp_finalize:
    case tp_traverse:
        return true;
    default:
        return false;
    }
}

static Handle typeFromSpec(Entry& entry, Handle metaclassHandle, Handle, TypeSpec* spec, Handle basesHandle)
{
    JSGlobalObject* globalObject = entry.globalObject;
    VM& vm = entry.vm;
    Runtime& rt = entry.runtime();

    // ---- What the extension says ----

    JSValue bases = toJS(basesHandle);
    const char* doc = nullptr;
    for (TypeSlot* slot = spec->slots; slot->slot; ++slot) {
        if (slot->slot == tp_base || slot->slot == tp_bases) {
            // The argument comes before Py_tp_bases, and that before Py_tp_base.
            if (!basesHandle && (!bases || slot->slot == tp_bases))
                bases = toJS(static_cast<Handle>(slot->value));
        } else if (slot->slot == tp_doc)
            doc = static_cast<const char*>(slot->value);
        else if (!isSlotOfLife(slot->slot)) {
            entry.raise(rt.SystemError(), makeString("PyType_FromSpec: slot "_s, slot->slot, " is not supported yet"_s));
            return nullptr;
        }
    }
    if (spec->flags & HaveGC) {
        bool hasTraverse = false;
        for (TypeSlot* slot = spec->slots; slot->slot; ++slot)
            hasTraverse |= slot->slot == tp_traverse && slot->value;
        if (!hasTraverse) {
            entry.raise(rt.SystemError(), makeString("type "_s, String::fromUTF8(spec->name), " has the Py_TPFLAGS_HAVE_GC flag but has no traverse function"_s));
            return nullptr;
        }
    }
    if (!bases)
        bases = rt.objectType();
    if (!PyTuple::is(bases) || !PyTuple::from(bases)->length())
        bases = PyTuple::create(globalObject, { PyTuple::is(bases) ? JSValue(rt.objectType()) : bases });
    // In abi3t C does not know how large an object is: a type can only say how much it adds.
    if (spec->basicSize > 0 || spec->itemSize) {
        entry.raise(rt.SystemError(), "PyType_FromSpec: in abi3t basicsize is what the type adds to its base, as a negative number, and itemsize is 0"_s);
        return nullptr;
    }
    // (So that the sizes of a chain of types add up in 32 bits.)
    if (spec->basicSize < -(1 << 24)) {
        entry.raise(rt.SystemError(), "PyType_FromSpec: basicsize is out of range"_s);
        return nullptr;
    }

    // All that can be refused is refused before there is a class: once made, its bases know of it.
    if (spec->flags & ImmutableTypeFlag) {
        for (JSValue base : PyTuple::from(bases)->span()) {
            if (PyType* type = PyType::tryFrom(base); type && !type->isImmutable()) {
                entry.raise(rt.TypeError(), makeString("Creating immutable type "_s, String::fromUTF8(spec->name), " from mutable base "_s, type->nameInMessages()));
                return nullptr;
            }
        }
    }
    PyType* base = findBestBase(globalObject, PyTuple::from(bases));
    if (entry.failed())
        return nullptr;
    MadeByExtension* ofBase = madeByExtension(base);
    // If not: it is as a class statement would have made it.
    bool isRemembered = ofBase || base->solidBase() == rt.objectType();
    if (!isRemembered) {
        bool saysSomething = spec->basicSize;
        for (TypeSlot* slot = spec->slots; slot->slot; ++slot)
            saysSomething |= isSlotOfLife(slot->slot);
        if (saysSomething) {
            entry.raise(rt.SystemError(), makeString("PyType_FromSpec: a type with data or a life of its own cannot derive from '"_s, base->name(), "' yet"_s));
            return nullptr;
        }
    }
    uint32_t dataOffset = ofBase ? ofBase->dataEnd : 0;
    uint32_t dataSize = static_cast<uint32_t>(-static_cast<int64_t>(spec->basicSize));
    uint32_t dataEnd = roundUpToMultipleOf<Body::dataAlignment>(dataOffset + dataSize);
    if (dataEnd > 1u << 30) {
        entry.raise(rt.SystemError(), "PyType_FromSpec: the instances would be too large"_s);
        return nullptr;
    }

    auto wholeName = unsafeSpan(spec->name);
    size_t dot = reverseFind(wholeName, '.');
    JSValue name = codecDecodeUTF8(globalObject, byteCast<uint8_t>(dot == notFound ? wholeName : wholeName.subspan(dot + 1)), "strict"_s);
    if (entry.failed())
        return nullptr;

    // ---- The class, as type(name, bases, namespace) makes it ----

    PyDict* namespaceOfClass = PyDict::create(globalObject);
    // The instances have neither a __dict__ nor weak references. (The name is taken out again below.)
    namespaceOfClass->set(vm, rt.names.dSlots(), rt.emptyTuple());
    if (dot != notFound) {
        JSValue module = codecDecodeUTF8(globalObject, byteCast<uint8_t>(wholeName.first(dot)), "strict"_s);
        if (entry.failed())
            return nullptr;
        namespaceOfClass->set(vm, rt.names.dModule(), module);
    } else
        namespaceOfClass->set(vm, rt.names.dModule(), makeStr(vm, "builtins"_s));
    if (doc) {
        JSValue text = codecDecodeUTF8(globalObject, byteCast<uint8_t>(unsafeSpan(doc)), "strict"_s);
        if (entry.failed())
            return nullptr;
        namespaceOfClass->set(vm, rt.names.dDoc(), text);
    }

    PyType* metaclass = metaclassHandle ? PyType::from(toJS(metaclassHandle)) : rt.typeType();
    JSValue made = makeTypeForExtension(globalObject, metaclass, name, bases, namespaceOfClass);
    if (entry.failed())
        return nullptr;
    PyType* type = PyType::from(made);
    ASSERT(type->base() == base);
    type->dict()->remove(vm, rt.names.dSlots());
    if (dot != notFound)
        type->setModuleInMessages(vm, asString(namespaceOfClass->get(rt.names.dModule())));

    // ---- What the C API remembers of it ----

    if (isRemembered) {
        auto* extension = MadeByExtension::create(vm, interpreter().state->madeByExtensionStructure.get());
        for (TypeSlot* slot = spec->slots; slot->slot; ++slot) {
            if (isSlotOfLife(slot->slot))
                extension->slots[slot->slot] = slot->value;
        }
        extension->flags = spec->flags | HeapTypeFlag;
        if (ofBase) {
            // What a type does not say it has from its base. That the collector is told of the references, how, and how they are let go of, go together.
            bool saysHowItIsCollected = (spec->flags & HaveGC) || extension->slots[tp_traverse] || extension->slots[tp_clear];
            for (Slot slot : { tp_dealloc, tp_finalize }) {
                if (!extension->slots[slot])
                    extension->slots[slot] = ofBase->slots[slot];
            }
            if (!saysHowItIsCollected) {
                extension->flags |= ofBase->flags & HaveGC;
                extension->slots[tp_traverse] = ofBase->slots[tp_traverse];
                extension->slots[tp_clear] = ofBase->slots[tp_clear];
            }
        }
        extension->dataOffset = dataOffset;
        extension->dataSize = dataSize;
        extension->dataEnd = dataEnd;
        type->setExtension(vm, extension);
        type->add(TypeFlag::InstancesHaveDataOfC);
        if (extension->dataSize) {
            type->add(TypeFlag::OwnLayout);
            type->setSolidBase(vm, type);
        }
    }
    if (!(spec->flags & BaseTypeFlag))
        type->remove(TypeFlag::Base);
    if (spec->flags & ImmutableTypeFlag)
        type->add(TypeFlag::Immutable);
    if (spec->flags & DisallowInstantiationFlag)
        type->disallowInstantiation();
    type->didChange(vm);
    return entry.newReference(type);
}

PY_CAPI Handle PyType_FromMetaclass(Handle metaclass, Handle module, TypeSpec* spec, Handle bases)
{
    CAPI_ENTER(nullptr);
    return typeFromSpec(entry, metaclass, module, spec, bases);
}

PY_CAPI Handle PyType_FromModuleAndSpec(Handle module, TypeSpec* spec, Handle bases)
{
    CAPI_ENTER(nullptr);
    return typeFromSpec(entry, nullptr, module, spec, bases);
}

PY_CAPI Handle PyType_FromSpecWithBases(TypeSpec* spec, Handle bases)
{
    CAPI_ENTER(nullptr);
    return typeFromSpec(entry, nullptr, nullptr, spec, bases);
}

PY_CAPI Handle PyType_FromSpec(TypeSpec* spec)
{
    CAPI_ENTER(nullptr);
    return typeFromSpec(entry, nullptr, nullptr, spec, nullptr);
}

// ---- Asking a type ----

PY_CAPI int PyType_IsSubtype(Handle derived, Handle base)
{
    CAPI_QUIET_AND_ONLY_READS(0);
    // (Not toJS(): that may have to make something.)
    PyType* derivedType = PyType::tryFrom(toLookAt(derived));
    PyType* baseType = PyType::tryFrom(toLookAt(base));
    return derivedType && baseType && derivedType->isSubtypeOf(baseType);
}

// Null, without an exception: the type has nothing there.
// (A tp_traverse asks here for the tp_traverse of the base, to which it leaves the rest.)
PY_CAPI void* PyType_GetSlot(Handle typeHandle, int slot)
{
    CAPI_QUIET_AND_ONLY_READS(nullptr);
    PyType* type = PyType::tryFrom(toLookAt(typeHandle));
    if (!type || slot < 0 || static_cast<unsigned>(slot) >= numberOfSlots) [[unlikely]] {
        if (t_isInTraverse)
            return nullptr;
        CAPI_ENTER_AFTER_ALL(nullptr);
        entry.raiseBadInternalCall();
        return nullptr;
    }
    MadeByExtension* extension = madeByExtension(type);
    if (void* given = extension ? extension->slots[slot] : nullptr)
        return given;
    // What every type has.
    switch (slot) {
    case tp_alloc:
        return reinterpret_cast<void*>(PyType_GenericAlloc);
    case tp_free:
        return reinterpret_cast<void*>(extension && (extension->flags & HaveGC) ? PyObject_GC_Del : PyObject_Free);
    default:
        return nullptr;
    }
}

PY_CAPI ssize_t PyType_GetTypeDataSize(Handle typeHandle)
{
    CAPI_QUIET(0);
    auto* made = MadeByExtension::of(PyType::from(toJS(typeHandle)));
    return made ? made->dataEnd - made->dataOffset : 0;
}

} // namespace CApi
} // namespace Python
} // namespace JSC

#endif // PYTHON_HAS_CAPI
