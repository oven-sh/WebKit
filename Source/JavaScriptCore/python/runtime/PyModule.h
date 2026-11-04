/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "PyDict.h"
#include "PyNative.h"

namespace JSC {
namespace Python {

class PyModule final : public PyObject {
public:
    PY_DECLARE_CELL(PyModule, PyObject, PyModuleType);
    DECLARE_VISIT_CHILDREN;

    static PyModule* create(VM&, Structure*, PyType*);
    // types.ModuleType(name): __name__ is set, and __doc__, __package__, __loader__ and __spec__ are None.
    static PyModule* create(JSGlobalObject*, JSString* name);
    static PyModule* create(JSGlobalObject*, ASCIILiteral name);

    PyDict* dict() const { return m_dict.get(); }
    void setDict(VM& vm, PyDict* dict) { m_dict.set(vm, this, dict); }
    static constexpr ptrdiff_t offsetOfDict() { return OBJECT_OFFSETOF(PyModule, m_dict); }

    void add(JSGlobalObject*, ASCIILiteral name, JSValue);
    void addFunctions(JSGlobalObject*, std::span<const NativeDef>);

    // Made by createNativeModule().
    bool isNative() const { return m_isNative; }
    void setIsNative() { m_isNative = true; }

private:
    PyModule(VM&, Structure*, PyType*);

    WriteBarrier<PyDict> m_dict;
    bool m_isNative { false };
};

// A module written in C++: what `import name` finds without looking at any file.
struct NativeModuleDef {
    ASCIILiteral name;
    // Fills the module, which has its name. What throws says so through the VM.
    void (*initialize)(JSGlobalObject*, PyModule*);
    // Runs at every import, when the module is in sys.modules, as the code of a module written in Python does. It may put something else there.
    void (*execute)(JSGlobalObject*, PyModule*) { nullptr };
};
std::span<const NativeModuleDef> nativeModules();
// More of them, from the host: those that need libraries JSC does not have (compression, hashes, databases). The table is
// never freed, and is set before Python first runs.
JS_EXPORT_PRIVATE void setHostNativeModules(std::span<const NativeModuleDef>);
std::span<const NativeModuleDef> hostNativeModules();
const NativeModuleDef* findNativeModule(StringView name);
// A module is initialized once for each interpreter: its types and functions are made then. No program gets to see the namespace that
// comes of it. Null: an exception.
PyDict* nativeModuleNamespace(JSGlobalObject*, JSString* name, const NativeModuleDef&);
// A new module object, which begins with a copy of that. So a module that is taken out of sys.modules and imported again is a new one,
// without what was done to the old one. (It is what CPython does for an extension module that cannot be initialized twice. Most of its
// own can, and make their types and functions anew.)
JSValue createNativeModule(JSGlobalObject*, JSString* name, const NativeModuleDef&);

// For the message when a module has not got a name that is asked of it: where it is from, and whether that is the trouble.
struct ModuleDiagnosis {
    JSValue origin; // The file, a str, if its __spec__ names one.
    bool mayHideAnother { false }; // It is in the directory of the script, so it comes before any other module of its name,
    bool hidesStandardLibrary { false }; // and the standard library has one.
};
// False: an exception.
bool diagnoseModule(JSGlobalObject*, JSValue spec, JSValue moduleName, ModuleDiagnosis&);

} // namespace Python
} // namespace JSC
