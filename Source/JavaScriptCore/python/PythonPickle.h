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

#pragma once

#include "PyDict.h"
#include "PyStateObject.h"
#include "PyType.h"
#include "PythonCodecs.h"
#include <wtf/HashMap.h>

// The module _pickle: Modules/_pickle.c of CPython, function for function. pickle is written in Python besides, and does without.
//
//     PythonPickleBuffer.cpp  pickle.PickleBuffer: Objects/picklebufobject.c
//     PythonPickler.cpp       Pickler, dump() and dumps()
//     PythonUnpickler.cpp     Unpickler, load() and loads()
//     PythonPickleModule.cpp  what they have in common, and the module

namespace JSC {

class PyMemoryView;

namespace Python {

constexpr int highestPickleProtocol = 5;
constexpr int defaultPickleProtocol = 5;

// They are to be kept the same as those of pickle.py. What each is for is in pickletools.py.
enum class Opcode : uint8_t {
    Mark = '(',
    Stop = '.',
    Pop = '0',
    PopMark = '1',
    Dup = '2',
    Float = 'F',
    Int = 'I',
    BinInt = 'J',
    BinInt1 = 'K',
    Long = 'L',
    BinInt2 = 'M',
    None = 'N',
    PersistentID = 'P',
    BinPersistentID = 'Q',
    Reduce = 'R',
    String = 'S',
    BinString = 'T',
    ShortBinString = 'U',
    Unicode = 'V',
    BinUnicode = 'X',
    Append = 'a',
    Build = 'b',
    Global = 'c',
    Dict = 'd',
    EmptyDict = '}',
    Appends = 'e',
    Get = 'g',
    BinGet = 'h',
    Inst = 'i',
    LongBinGet = 'j',
    List = 'l',
    EmptyList = ']',
    Obj = 'o',
    Put = 'p',
    BinPut = 'q',
    LongBinPut = 'r',
    SetItem = 's',
    Tuple = 't',
    EmptyTuple = ')',
    SetItems = 'u',
    BinFloat = 'G',

    // Protocol 2
    Proto = 0x80,
    NewObj = 0x81,
    Ext1 = 0x82,
    Ext2 = 0x83,
    Ext4 = 0x84,
    Tuple1 = 0x85,
    Tuple2 = 0x86,
    Tuple3 = 0x87,
    NewTrue = 0x88,
    NewFalse = 0x89,
    Long1 = 0x8a,
    Long4 = 0x8b,

    // Protocol 3
    BinBytes = 'B',
    ShortBinBytes = 'C',

    // Protocol 4
    ShortBinUnicode = 0x8c,
    BinUnicode8 = 0x8d,
    BinBytes8 = 0x8e,
    EmptySet = 0x8f,
    AddItems = 0x90,
    FrozenSet = 0x91,
    NewObjEx = 0x92,
    StackGlobal = 0x93,
    Memoize = 0x94,
    Frame = 0x95,

    // Protocol 5
    ByteArray8 = 0x96,
    NextBuffer = 0x97,
    ReadOnlyBuffer = 0x98,
};

constexpr unsigned pickleBatchSize = 1000; // BATCHSIZE: how many are written before APPENDS or SETITEMS. The same as pickle.Pickler._BATCHSIZE.
constexpr int fastNestingLimit = 50; // FAST_NESTING_LIMIT: how deep, with no memo, before it is looked into whether something has itself in it
constexpr size_t picklePrefetch = 8192 * 16; // PREFETCH: how much is asked for ahead of what is wanted, of what can be looked ahead in
constexpr size_t frameSizeMinimum = 4;
constexpr size_t frameSizeTarget = 64 * 1024;
constexpr size_t frameHeaderSize = 9;

// PickleState
struct PickleModuleState final : NativeState {
    PYTHON_NATIVE_STATE(PickleModuleState);
    WriteBarrier<PyType> pickleError;
    WriteBarrier<PyType> picklingError;
    WriteBarrier<PyType> unpicklingError;
    WriteBarrier<PyDict> dispatchTable; // copyreg.dispatch_table, { class: function }
    // For EXT1, EXT2 and EXT4
    WriteBarrier<PyDict> extensionRegistry; // copyreg._extension_registry, { (module, name): code }
    WriteBarrier<PyDict> extensionCache; // copyreg._extension_cache, { code: object }
    WriteBarrier<PyDict> invertedRegistry; // copyreg._inverted_registry, { code: (module, name) }
    // What things were called in Python 2
    WriteBarrier<PyDict> nameMapping2To3; // _compat_pickle.NAME_MAPPING, { (module, name): (module, name) }
    WriteBarrier<PyDict> importMapping2To3; // _compat_pickle.IMPORT_MAPPING, { module: module }
    WriteBarrier<PyDict> nameMapping3To2;
    WriteBarrier<PyDict> importMapping3To2;
    WriteBarrier<Unknown> codecsEncode; // For bytes, where there is no way to write one
    WriteBarrier<Unknown> getattr; // For what is inside something else, before protocol 4
    WriteBarrier<Unknown> partial; // For __newobj_ex__, in protocols 2 and 3
    WriteBarrier<PyType> picklerType;
    WriteBarrier<PyType> unpicklerType;
    WriteBarrier<PyType> picklerMemoProxyType;
    WriteBarrier<PyType> unpicklerMemoProxyType;
    WriteBarrier<PyType> pickleBufferType;
};

template<typename Visitor>
void PickleModuleState::visit(Visitor& visitor)
{
    visitor.append(pickleError);
    visitor.append(picklingError);
    visitor.append(unpicklingError);
    visitor.append(dispatchTable);
    visitor.append(extensionRegistry);
    visitor.append(extensionCache);
    visitor.append(invertedRegistry);
    visitor.append(nameMapping2To3);
    visitor.append(importMapping2To3);
    visitor.append(nameMapping3To2);
    visitor.append(importMapping3To2);
    visitor.append(codecsEncode);
    visitor.append(getattr);
    visitor.append(partial);
    visitor.append(picklerType);
    visitor.append(unpicklerType);
    visitor.append(picklerMemoProxyType);
    visitor.append(unpicklerMemoProxyType);
    visitor.append(pickleBufferType);
}

PickleModuleState& pickleModuleState(JSGlobalObject*);

// get_dotted_path() and getattribute(). The first appends the parts of a name, between the dots. The second is empty if there is no such thing, which has been raised if `raises`.
void appendDottedPath(JSGlobalObject*, JSValue name, MarkedArgumentBuffer&);
JSValue getAttributeByPath(JSGlobalObject*, JSValue object, const MarkedArgumentBuffer& names, bool raises);

// ---- pickle.PickleBuffer

void initializePickleBufferType(JSGlobalObject*);
bool isPickleBuffer(JSGlobalObject*, JSValue); // PyPickleBuffer_Check()

// ---- Each adds its classes and functions to the module.

void initializePickler(JSGlobalObject*);
void initializeUnpickler(JSGlobalObject*);
void addPicklerFunctions(JSGlobalObject*, JSObject* module);
void addUnpicklerFunctions(JSGlobalObject*, JSObject* module);

} } // namespace JSC::Python
