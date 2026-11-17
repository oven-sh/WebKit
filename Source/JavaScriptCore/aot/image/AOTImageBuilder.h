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
#include "AOTPrefixCode.h"
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
    Vector<uint32_t> sizes;
    Vector<uint8_t> positions;
    Vector<uint32_t> frameMapWords;
    Vector<ImageFrame> frames;
};

struct WebAssemblyImageInputs {
    Vector<uint8_t> code;
    size_t codeAlignment { 1 };
    Vector<uint8_t> directory;
    uint32_t numberOfModules { 0 };
};

class EarlyWasmModules {
public:
    virtual ~EarlyWasmModules() = default;
};

struct ImageContents {
    uint32_t numberOfRegExpsWithCode { 0 };
    uint32_t numberOfRegExpsWithoutCode { 0 };
    uint32_t numberOfRegExpsWithUnicodePropertyLookup { 0 };
    uint32_t numberOfWebAssemblyModules { 0 };
};

struct ImageInputs {
    Typed::Program& program;
    const Typed::Facts& facts;
    const Typed::Plan& plan;
    const StubCatalog& stubCatalog;
    LinkedFunctions functions;
    BodyFactsForTables bodyFacts;
    WTF::BitSet<numberOfEntries> entriesUsedByFunctions;
    std::unique_ptr<EarlyWasmModules> earlyWasmModules { };
};

struct BuiltImage {
    Vector<uint8_t> image;
    WTF::BitSet<numberOfEntries> entriesUsed;
    ImageContents contents;
    LinkedFunctions functions;
    std::unique_ptr<ImageNumbers> numbers;
    CodeLayout layout;
    SharedCode sharedCode;
    Vector<uint32_t> sideTableBytes;
    std::array<uint64_t, numberOfTableParts> bytesOfTableParts { };
};

BuiltImage buildImage(ImageInputs&&, BuildContext&);

void reportLinkFailure(LinkFailure, FunctionID, BuildContext&);
OptionSet<ImageFlag> imageFlagsAdmittedBy(Wire::UnseenCodeCause);
OptionSet<ImageFlag> flagsForUnseenCode(const Typed::Facts&, const CompileOptions&);
ResolvedCallees resolveCallees(const LinkedFunctions&, const Typed::Plan&, BuildContext&);
SideTables encodeSideTables(const LinkedFunctions&, std::span<const Vector<PrefixSymbol>> callSites, BuildContext&);
struct CalledSharedCode {
    Vector<StubKey> stubKeys;
    Vector<StubKey> stubKeysOfArgumentMoves;
    Vector<KeptArgumentMoves> argumentMoves;
    Vector<uint32_t> firstArgumentMovesOfBody;
    Vector<uint32_t> thunksOfArgumentMoves;
};
std::optional<CalledSharedCode> collectCalledSharedCode(const LinkedFunctions&, BuildContext&);
SharedCode generateSharedCode(CalledSharedCode&&, const StubCatalog&, const WTF::BitSet<numberOfEntries>& entriesUsedByFunctions, BuildContext&);
SharedCode selectSharedCode(const LinkedFunctions&, const StubCatalog&, const WTF::BitSet<numberOfEntries>& entriesUsedByFunctions, BuildContext&);
void applyNumbersToFunctions(LinkedFunctions&, const ImageNumbers&, BuildContext&);
void checkUncheckedStackUsage(const LinkedFunctions&, const ResolvedCallees&, BuildContext&);
JS_EXPORT_PRIVATE WebAssemblyImageInputs compileWebAssemblyModules(const Typed::Program&, const CodeTemplates&, BuildContext&, const BitVector* stringsInImage = nullptr, std::unique_ptr<EarlyWasmModules> = nullptr, const LinkedFunctions* = nullptr);
JS_EXPORT_PRIVATE std::unique_ptr<EarlyWasmModules> compileWebAssemblyFiles(const Typed::Program&, BuildContext&);
Vector<uint8_t> encodeSymbols(const Typed::Program&, const BuiltImage&, BuildContext&);
void writeMapFile(const UTF8CString& path, const Typed::Program&, const BuiltImage&, BuildContext&);
Vector<uint8_t> encodeRowsOfMapFile(const Typed::Program&, const BuiltImage&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
