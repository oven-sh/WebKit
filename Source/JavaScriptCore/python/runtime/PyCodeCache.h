/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "JSCJSValue.h"
#include <span>
#include <wtf/Vector.h>
#include <wtf/text/WTFString.h>

namespace JSC {

class JSGlobalObject;

namespace Python {

class PyBytes;
class PyCode;

// Compiled code outside the process: what marshal writes for a code object, and so what is in a .pyc file.
//
// The code is JSC's bytecode, in the form JSC has for keeping bytecode in files (runtime/CachedTypes.cpp), which is read in
// place: a function in it costs next to nothing until it is called. It means something to the build that wrote it and to no
// other, so it starts with a number that tells builds apart. The text the code was compiled from goes with it.

// sys.implementation.cache_tag: what the name of a .pyc file has between the name of the module and ".pyc".
inline ASCIILiteral codeCacheTag() { return "bun-315"_s; }

// Different in every build of the program that JSC is part of.
uint64_t codeCacheStamp();
// The first four bytes of a .pyc file, as an int (_imp.pyc_magic_number_token): made from the stamp, so that importlib takes a
// file that another build wrote for out of date.
uint32_t pycMagicNumber();

// _imp.source_hash(): by which a .pyc file that goes by the contents of its source knows them again.
uint64_t hashOfSource(uint64_t key, std::span<const uint8_t>);

// The type code in marshal's format ('c' is CPython's), and what follows it.
static constexpr uint8_t marshalTypeOfCode = 'C';
struct MarshalledCodeHeader {
    static constexpr size_t size = 28;
    uint64_t stamp;
    uint64_t checksum; // Of the payload and the flags.
    uint32_t payloadSize;
    uint32_t codeFlags; // co_flags, which code.replace() can change.
    uint32_t reserved;

    void write(Vector<uint8_t>&) const;
    static MarshalledCodeHeader read(std::span<const uint8_t, size>);
};

// Appends the header and the payload. False: there is no writing this one. No exception.
bool writeMarshalledCode(JSGlobalObject*, PyCode*, Vector<uint8_t>&);
// Whether the payload is one this build can read is for these two to say. Neither raises anything.
bool isOfThisBuild(const MarshalledCodeHeader&);
// `payload` has the size the header says. Null: it is not what was written.
PyCode* readMarshalledCode(JSGlobalObject*, const MarshalledCodeHeader&, std::span<const uint8_t> payload);

// code.co_filename = path, and the same for all the code in it (_imp._fix_co_filename).
void setCodeFilename(JSGlobalObject*, PyCode*, const String& path);

// The code of a module in a file, out of its .pyc file if that is up to date, which is written otherwise: what importlib does
// (SourceLoader.get_code), for the modules that are loaded before importlib is. `shownFileName` is the co_filename.
// Empty without an exception: there is no such file.
JSValue codeOfSourceFile(JSGlobalObject*, const String& path, const String& shownFileName);

// Whether these are the contents of a .pyc file of this build's, going by how they start, and the code in them. Null: there is none.
bool hasPycMagicNumber(std::span<const uint8_t>);
PyCode* codeInPycFile(JSGlobalObject*, std::span<const uint8_t>);

// The contents of a .pyc file, for _io.open_code(). Null: errno says why not. One reason is that the code in it is not what was
// written (EIO): such a file is as good as none.
PyBytes* readPycFile(JSGlobalObject*, const String& path);

} // namespace Python
} // namespace JSC
