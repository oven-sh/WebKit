/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCodeLayout.h"
#include "AOTConstantPool.h"
#include "AOTFFIStubs.h"
#include "AOTImageFormat.h"
#include "AOTLinkedFunctions.h"
#include "AOTOutliner.h"
#include "AOTRegExpCompiler.h"
#include "AOTTextEncoder.h"
#include "AOTVector.h"
#include <span>
#include <wtf/BitSet.h>
#include <wtf/text/CString.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT {

class StubCatalog;

struct SideTables {
    Vector<uint8_t> bytes;
    Vector<uint32_t> offsets;
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

struct ImageInputs {
    const Typed::PipelineOutput& pipeline;
    const StubCatalog& stubCatalog;
    InstanceSizes instanceSizes;
    LinkedFunctions functions;
    WTF::BitSet<numberOfEntries> entriesUsedByFunctions;
    Vector<uint8_t> programTables;
    EncodedText text;
    const RegExpRequests& regExpRequests;
    CompiledRegExps regExps;
    WebAssemblyImageInputs webAssembly;
    FFIStubs ffiStubs;
};

struct BuiltImage {
    Vector<uint8_t> image;
    WTF::BitSet<numberOfEntries> entriesUsed;
    LinkedFunctions functions;
    Vector<EntryAlias> aliases;
    Vector<FoldedFunction> foldedFunctions;
    Outlining outlining;
    OutlinedCode outlinedCode;
    SharedCode sharedCode;
    ConstantPool constants;
    RegExpSection regExpSection;
    CodeLayout layout;
};

struct ImageSymbol {
    enum class Kind : uint8_t {
        Function,
        FoldedFunction,
        SharedCodeCopy,
        SharedCode,
        OwnOutlinedBody,
        RegExpBody,
        RegExpRoutine,
        RegExpSection,
        WebAssemblyCode,
        Constants,
        FFICode,
    };

    Kind kind;
    uint32_t offset;
    uint32_t size;
    uint32_t index { 0 };
    FunctionID function { };
    String name { };
    String description { };
};

struct ImageSymbolInputs {
    const BuiltImage& built;
    const Typed::Program& program;
    const Typed::Heat& heat;
    const RegExpRequests& regExpRequests;
    UTF8CString arrivalSites { };
};

void reportLinkFailure(LinkFailure, FunctionID, BuildContext&);
BuiltImage buildImage(ImageInputs&&, BuildContext&);
OptionSet<ImageFlag> flagsForUnseenCode(const Typed::PipelineOutput&);
void orderFunctions(LinkedFunctions&, const Typed::PipelineOutput&, BuildContext&);
ResolvedCallees resolveCallees(const LinkedFunctions&, const CodeIndex&, std::span<const EntryAlias>, BuildContext&);
SideTables encodeSideTables(const LinkedFunctions&, std::span<const Vector<uint8_t>> callSites, BuildContext&);
void removeJumpOnlyFunctions(LinkedFunctions&, Vector<EntryAlias>&, const CodeTemplates&, BuildContext&);
Vector<FoldedFunction> foldIdenticalFunctions(LinkedFunctions&, Vector<EntryAlias>&, const Typed::Program&, BuildContext&);
void checkUncheckedStackUsage(const LinkedFunctions&, const ResolvedCallees&, std::span<const EntryAlias>, const CodeIndex&, BuildContext&);
SharedCode selectSharedCode(const LinkedFunctions&, const OutlinedCode&, const StubCatalog&, const WTF::BitSet<numberOfEntries>& entriesUsedByFunctions, BuildContext&);

Vector<ImageSymbol> collectSymbols(const ImageSymbolInputs&);
Vector<uint8_t> encodeSymbols(std::span<const ImageSymbol>, std::span<const uint8_t> image);
void writeMapFile(const UTF8CString& path, std::span<const ImageSymbol>, const ImageSymbolInputs&, std::span<const uint8_t> image, const BuildContext&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
