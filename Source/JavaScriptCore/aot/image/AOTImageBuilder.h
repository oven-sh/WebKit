/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBodyFactsForTables.h"
#include "AOTCodeLayout.h"
#include "AOTConstantPool.h"
#include "AOTFFIStubs.h"
#include "AOTImageFormat.h"
#include "AOTLinkedFunctions.h"
#include "AOTNumbering.h"
#include "AOTRegExpCompiler.h"
#include "AOTTableParts.h"
#include "AOTTextEncoder.h"
#include "AOTVector.h"
#include "AOTWire.h"
#include <array>
#include <memory>
#include <span>
#include <wtf/BitSet.h>
#include <wtf/text/CString.h>

namespace JSC { namespace AOT {

class StubCatalog;

struct SideTables {
    Vector<uint8_t> bytes;
    Vector<uint32_t> offsets;
    Vector<OptionSet<ImageFunctionFlag>> flags;
    Vector<uint8_t> positions;
    Vector<uint32_t> frameMapWords;
    Vector<ImageFrame> frames;
};

// The code of the modules of the program, and the directory that Wasm::CodeInImage reads. A module without code is listed there as declined, so that the run time does not look further.
struct WebAssemblyImageInputs {
    Vector<uint8_t> code;
    size_t codeAlignment { 1 };
    Vector<uint8_t> directory;
    uint32_t numberOfModules { 0 };
};

struct ImageContents {
    uint32_t numberOfRegExpsWithCode { 0 };
    uint32_t numberOfRegExpsWithoutCode { 0 };
    uint32_t numberOfRegExpsWithUnicodePropertyLookup { 0 };
    uint32_t numberOfWebAssemblyModules { 0 };
};

// The program is not const: Image adds strings to it (pieces of paths, texts that the walk made, atoms and group names of regular expressions) in front of numbering them.
struct ImageInputs {
    Typed::Program& program;
    const Typed::Facts& facts;
    const Typed::Plan& plan;
    const StubCatalog& stubCatalog;
    LinkedFunctions functions;
    BodyFactsForTables bodyFacts;
    WTF::BitSet<numberOfEntries> entriesUsedByFunctions;
};

struct BuiltImage {
    Vector<uint8_t> image;
    WTF::BitSet<numberOfEntries> entriesUsed;
    ImageContents contents;
    LinkedFunctions functions;
    std::unique_ptr<ImageNumbers> numbers;
    CodeLayout layout;
    SharedCode sharedCode;
    // [position]: the bytes of the call sites, positions, handlers and frame ranges of the function.
    Vector<uint32_t> sideTableBytes;
    // [TablePartID]
    std::array<uint64_t, numberOfTableParts> bytesOfTableParts { };
};

// The stage, in this order. What `|` separates are lanes (runLanes(), AOTThreads.h): none reads what another writes, and each has loops on all threads. In a lane `>` separates a step from the one that waits for it.
//   hide paths of modules, intern pieces of paths > intern the texts that functions made | resolve callees
//   gather mentions
//   name what the tables name
//   number
//   give each function its id, each numbered stub key its immediate and each mentioned throw word its number (FaultingAccess::wordsThatAreMentions, ArgumentSource::ThrowWordWithMention)
//   index functions, choose the code granule, generate FFI stubs, compile WebAssembly modules, find final jumps and far words of the instance
//     | encode program tables > build constant pool
//     | collect what the functions call of the shared code > generate the shared code
//     | compile regular expressions > lay out their section > encode their records
//     | encode text
//     | encode call sites > encode side tables
//   lay out code
//   remove final jumps | encode function tables | encode fault sites
//   write the image: copy the sections, write the strings, link the code
BuiltImage buildImage(ImageInputs&&, BuildContext&);

void reportLinkFailure(LinkFailure, FunctionID, BuildContext&);
// What the run time lets in where Facts lists a record of the cause. It turns away whatever no record lets in.
OptionSet<ImageFlag> imageFlagsAdmittedBy(Wire::UnseenCodeCause);
OptionSet<ImageFlag> flagsForUnseenCode(const Typed::Facts&, const CompileOptions&);
// [position][index in CodeBody::callees]. A callee that a relocation names and that has no code is an error of the compiler.
ResolvedCallees resolveCallees(const LinkedFunctions&, const Typed::Plan&, BuildContext&);
SideTables encodeSideTables(const LinkedFunctions&, std::span<const Vector<uint8_t>> callSites, BuildContext&);
struct CalledSharedCode {
    // In no order, with repeats.
    Vector<StubKey> stubKeys;
    // Each distinct list once, ascending by its words. They point into the functions.
    Vector<const ArgumentMoves*> argumentMoves;
};
// Both steps of selectSharedCode(). The second has one assembler, and so one thread for all but its sort.
// Nothing where a function names a stub that does not exist, which is reported.
std::optional<CalledSharedCode> collectCalledSharedCode(const LinkedFunctions&, BuildContext&);
SharedCode generateSharedCode(CalledSharedCode&&, const StubCatalog&, const WTF::BitSet<numberOfEntries>& entriesUsedByFunctions, BuildContext&);
SharedCode selectSharedCode(const LinkedFunctions&, const StubCatalog&, const WTF::BitSet<numberOfEntries>& entriesUsedByFunctions, BuildContext&);
// CompiledCode::function, the immediates of CodeBody::numberedStubKeys, and the throw words that are mentions: in FaultingAccess::throwSite and in CodeBody::argumentMoves.
void applyNumbersToFunctions(LinkedFunctions&, const ImageNumbers&, BuildContext&);
// Checking builds only.
void checkUncheckedStackUsage(const LinkedFunctions&, const ResolvedCallees&, BuildContext&);
// AOTWasmModules.cpp. Every function of every module, on all threads. `stringsInImage`, by StringID: the strings whose text may hold a module. Without it every string may.
JS_EXPORT_PRIVATE WebAssemblyImageInputs compileWebAssemblyModules(const Typed::Program&, const CodeTemplates&, BuildContext&, const BitVector* stringsInImage = nullptr);
// AOTImageSymbols.cpp. ReportSet::Symbols: a name and a range of the code for each unit, stub, thunk and regular expression, in the order of the code.
// BuiltImage::image still holds the image: the header and the records of regular expressions are read there.
Vector<uint8_t> encodeSymbols(const Typed::Program&, const BuiltImage&, BuildContext&);
// AOTImageSymbols.cpp. LoggingOptions::mapFilePath: a row of text for each unit, stub, thunk and regular expression, for the tools of Bun's test/aot. Writes nothing where the path is empty.
// Called once for an image: each further call with the same path writes a file of its own, with the ending `.1`, `.2` ..
void writeMapFile(const UTF8CString& path, const Typed::Program&, const BuiltImage&, BuildContext&);
// What writeMapFile() writes. BuiltImage::image still holds the image, as for encodeSymbols().
Vector<uint8_t> encodeRowsOfMapFile(const Typed::Program&, const BuiltImage&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
