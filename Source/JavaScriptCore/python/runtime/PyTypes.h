/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace JSC {

class JSGlobalObject;

namespace Python {

// Each fills the built-in types its file implements. The runtime has made them all, bare, by then; a type is filled after its base.

void initializeObjectTypes(JSGlobalObject*); // object, type
void initializeSingletonTypes(JSGlobalObject*); // NoneType, NotImplementedType, ellipsis
void initializeIntTypes(JSGlobalObject*); // int, bool
void initializeFloatTypes(JSGlobalObject*); // float, complex
void initializeStrTypes(JSGlobalObject*);
void initializeBytesTypes(JSGlobalObject*); // bytes, bytearray
void initializeListTypes(JSGlobalObject*);
void initializeTupleTypes(JSGlobalObject*);
void initializeDictTypes(JSGlobalObject*); // dict and its views
void initializeSetTypes(JSGlobalObject*); // set, frozenset
void initializeRangeTypes(JSGlobalObject*); // range, slice
void initializeFunctionTypes(JSGlobalObject*); // function, method, cell, code, and the kinds of native function
void initializeDescriptorTypes(JSGlobalObject*); // property, classmethod, staticmethod, super, mappingproxy, getset_descriptor, member_descriptor
void initializeIteratorTypes(JSGlobalObject*);
void initializeGeneratorTypes(JSGlobalObject*);
void initializeModuleTypes(JSGlobalObject*);
void initializeFrameTypes(JSGlobalObject*); // frame, traceback
void initializeExceptionTypes(JSGlobalObject*);
void initializeExceptionGroupTypes(JSGlobalObject*);
void initializeGenericAliasTypes(JSGlobalObject*); // types.GenericAlias, types.UnionType
void initializeNamespaceTypes(JSGlobalObject*); // sentinel, types.SimpleNamespace
void initializeTypingTypes(JSGlobalObject*); // TypeVar and the like, TypeAliasType, Generic
void initializeTemplateTypes(JSGlobalObject*); // string.templatelib.Template, Interpolation
void initializeMemoryViewTypes(JSGlobalObject*); // memoryview
void initializeWeakRefTypes(JSGlobalObject*); // weakref.ref, weakref.proxy
void initializeInteropTypes(JSGlobalObject*); // What JS objects and functions are to Python.

} // namespace Python
} // namespace JSC
