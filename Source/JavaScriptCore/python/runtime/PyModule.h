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

private:
    PyModule(VM&, Structure*, PyType*);

    WriteBarrier<PyDict> m_dict;
};

// A module written in C++: what `import name` finds without looking at any file.
struct NativeModuleDef {
    ASCIILiteral name;
    // Fills the module, which has its name. What throws says so through the VM.
    void (*initialize)(JSGlobalObject*, PyModule*);
};
std::span<const NativeModuleDef> nativeModules();
// More of them, from the host: those that need libraries JSC does not have (compression, hashes, databases). The table is
// never freed, and is set before Python first runs.
JS_EXPORT_PRIVATE void setHostNativeModules(std::span<const NativeModuleDef>);
std::span<const NativeModuleDef> hostNativeModules();
const NativeModuleDef* findNativeModule(StringView name);
JSValue nativeModuleInstance(JSGlobalObject*, JSString* name, const NativeModuleDef&);

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
