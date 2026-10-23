/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCodeIndex.h"
#include "AOTCodeLayout.h"
#include "AOTCodeLinker.h"
#include "AOTConstantPool.h"
#include "AOTFFIStubs.h"
#include "AOTImageFormat.h"
#include "AOTLinkedFunctions.h"
#include "AOTOutliner.h"
#include "AOTRegExpCompiler.h"
#include "AOTRegExpSection.h"
#include "AOTTextEncoder.h"
#include "AOTVector.h"
#include <array>
#include <span>
#include <wtf/BitSet.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

class StubCatalog;

struct SideTables {
    Vector<uint8_t> bytes;
    Vector<uint32_t> offsets;
    Vector<uint8_t> positions;
    Vector<uint32_t> frameMapWords;
    Vector<ImageFrame> frames;
};

struct ImageSections {
    Vector<uint8_t>& operator[](ImageSection section) { return bytes[static_cast<size_t>(section)]; }
    const Vector<uint8_t>& operator[](ImageSection section) const { return bytes[static_cast<size_t>(section)]; }

    std::array<Vector<uint8_t>, numberOfImageSections> bytes;
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

// Address space in the code of an image that the run time maps memory of its own over. offset counts from the start of the code.
struct ReservedRange {
    uint64_t offset;
    uint64_t size;
};

struct ImageWriterInputs {
    const CodeLinkerInputs& linker;
    const ImageSections& sections;
    std::span<const uint8_t> webAssemblyCode;
    std::span<const uint8_t> ffiCode;
    std::span<const ReservedRange> reservedRanges;
    const WTF::BitSet<numberOfEntries>& entriesUsed;
    ImageCounts counts;
    OptionSet<ImageFlag> flags;
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

Vector<uint8_t> writeImage(const ImageWriterInputs&, BuildContext&);

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
