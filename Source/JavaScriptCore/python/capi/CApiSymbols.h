/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

// The data that the stable ABI names, as far as it stands for objects.

// v(symbol, id): &symbol is the built-in type Runtime::id##Type().
#define FOR_EACH_CAPI_STATIC_TYPE(v) \
    v(PyBaseObject_Type, object) \
    v(PyBool_Type, bool) \
    v(PyByteArrayIter_Type, bytearrayIterator) \
    v(PyByteArray_Type, bytearray) \
    v(PyBytesIter_Type, bytesIterator) \
    v(PyBytes_Type, bytes) \
    v(PyCFunction_Type, nativeFunction) \
    v(PyCallIter_Type, callableIterator) \
    v(PyClassMethodDescr_Type, classMethodDescriptor) \
    v(PyComplex_Type, complex) \
    v(PyDictItems_Type, dictItems) \
    v(PyDictIterItem_Type, dictItemIterator) \
    v(PyDictIterKey_Type, dictKeyIterator) \
    v(PyDictIterValue_Type, dictValueIterator) \
    v(PyDictKeys_Type, dictKeys) \
    v(PyDictProxy_Type, mappingProxy) \
    v(PyDictRevIterItem_Type, dictReverseItemIterator) \
    v(PyDictRevIterKey_Type, dictReverseKeyIterator) \
    v(PyDictRevIterValue_Type, dictReverseValueIterator) \
    v(PyDictValues_Type, dictValues) \
    v(PyDict_Type, dict) \
    v(PyEllipsis_Type, ellipsis) \
    v(PyEnum_Type, enumerate) \
    v(PyFilter_Type, filter) \
    v(PyFloat_Type, float) \
    v(PyFrozenSet_Type, frozenset) \
    v(PyGetSetDescr_Type, getSetDescriptor) \
    v(PyListIter_Type, listIterator) \
    v(PyListRevIter_Type, listReverseIterator) \
    v(PyList_Type, list) \
    v(PyLongRangeIter_Type, longRangeIterator) \
    v(PyLong_Type, int) \
    v(PyMap_Type, map) \
    v(PyMemberDescr_Type, memberDescriptor) \
    v(PyMemoryView_Type, memoryview) \
    v(PyMethodDescr_Type, methodDescriptor) \
    v(PyModule_Type, module) \
    v(PyProperty_Type, property) \
    v(PyRangeIter_Type, rangeIterator) \
    v(PyRange_Type, range) \
    v(PyReversed_Type, reversed) \
    v(PySeqIter_Type, sequenceIterator) \
    v(PySetIter_Type, setIterator) \
    v(PySet_Type, set) \
    v(PySlice_Type, slice) \
    v(PySuper_Type, super) \
    v(PyTraceBack_Type, traceback) \
    v(PyTupleIter_Type, tupleIterator) \
    v(PyTuple_Type, tuple) \
    v(PyType_Type, type) \
    v(PyUnicodeIter_Type, strIterator) \
    v(PyUnicode_Type, str) \
    v(PyWrapperDescr_Type, wrapperDescriptor) \
    v(PyZip_Type, zip) \
    v(Py_GenericAliasType, genericAlias) \
    v(_PyWeakref_CallableProxyType, weakCallableProxy) \
    v(_PyWeakref_ProxyType, weakProxy) \
    v(_PyWeakref_RefType, weakRef)

// v(name): the variable PyExc_##name holds Runtime::name().
#define FOR_EACH_CAPI_EXCEPTION(v) \
    v(ArithmeticError) \
    v(AssertionError) \
    v(AttributeError) \
    v(BaseException) \
    v(BaseExceptionGroup) \
    v(BlockingIOError) \
    v(BrokenPipeError) \
    v(BufferError) \
    v(BytesWarning) \
    v(ChildProcessError) \
    v(ConnectionAbortedError) \
    v(ConnectionError) \
    v(ConnectionRefusedError) \
    v(ConnectionResetError) \
    v(DeprecationWarning) \
    v(EOFError) \
    v(EncodingWarning) \
    v(Exception) \
    v(FileExistsError) \
    v(FileNotFoundError) \
    v(FloatingPointError) \
    v(FutureWarning) \
    v(GeneratorExit) \
    v(ImportError) \
    v(ImportWarning) \
    v(IndentationError) \
    v(IndexError) \
    v(InterruptedError) \
    v(IsADirectoryError) \
    v(KeyError) \
    v(KeyboardInterrupt) \
    v(LookupError) \
    v(MemoryError) \
    v(ModuleNotFoundError) \
    v(NameError) \
    v(NotADirectoryError) \
    v(NotImplementedError) \
    v(OSError) \
    v(OverflowError) \
    v(PendingDeprecationWarning) \
    v(PermissionError) \
    v(ProcessLookupError) \
    v(RecursionError) \
    v(ReferenceError) \
    v(ResourceWarning) \
    v(RuntimeError) \
    v(RuntimeWarning) \
    v(StopAsyncIteration) \
    v(StopIteration) \
    v(SyntaxError) \
    v(SyntaxWarning) \
    v(SystemError) \
    v(SystemExit) \
    v(TabError) \
    v(TimeoutError) \
    v(TypeError) \
    v(UnboundLocalError) \
    v(UnicodeDecodeError) \
    v(UnicodeEncodeError) \
    v(UnicodeError) \
    v(UnicodeTranslateError) \
    v(UnicodeWarning) \
    v(UserWarning) \
    v(ValueError) \
    v(Warning) \
    v(ZeroDivisionError)
