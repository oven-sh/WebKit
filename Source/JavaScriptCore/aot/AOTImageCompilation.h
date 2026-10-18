/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTImage.h"
#include "AOTTypedPasses.h"
#include "CachedTypes.h"
#include "SourceCode.h"
#include <array>
#include <span>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

struct ImageCompilation {
    using BuildError = BytecodeLinkEncoder::BuildError;

    struct Module {
        JSCell* root;
        SourceCode source;
        uint32_t entryOffset;
        bool isLate;
    };

    struct FunctionToCompile {
        unsigned module;
        OrderFunctionKey key;
        UnlinkedFunctionExecutable* executable;
        UnlinkedFunctionCodeBlock* forCall;
        UnlinkedFunctionCodeBlock* forConstruct;
        unsigned sourceStart;
    };

    ImageCompilation(VM& vm, EncoderStringTable* strings)
        : vm(vm)
        , strings(strings)
    {
    }

    Vector<uint8_t> compileImage();
    std::optional<Vector<uint32_t>> variablesExportedBy(unsigned index);

    void fail(String message, String path = { }, LineColumn position = { }) { buildErrors.append(BuildError { WTF::move(message), WTF::move(path), position }); }

    VM& vm;
    EncoderStringTable* strings;
    Vector<Module> modules;
    bool hasHotFunctions { false };
    std::array<unsigned, 3> omittedFunctions { };
    std::span<const uint8_t> prelinkedGraph;
    std::span<const uint32_t> prelinkedGraphStringSlots;
    std::span<const uint32_t> graphModuleIndices;
    std::unique_ptr<Typed::Program> typedProgram;
    std::unique_ptr<DiscoveredRegExps> discoveredRegExps;
    Vector<uint32_t, 1> typedModules;
    Vector<FunctionToCompile> functionsToCompile;
    Vector<FunctionReportableSites> reportableSites;
    Vector<BuildError> buildErrors;

private:
    void layOutModuleEnvironments();
    String pathOf(size_t module) const
    {
        auto& url = modules[module].source.provider()->sourceOrigin().url();
        return url.protocolIsFile() ? url.fileSystemPath() : url.string();
    }

    Vector<ImageEnvironment> linkEnvironments;
    size_t linkEnvironmentsSize { 0 };
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
