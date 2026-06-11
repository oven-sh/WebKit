/*
 * Copyright (C) 2013-2023 Apple Inc. All rights reserved.
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

#if ENABLE(FTL_JIT)

#include "ArrayProfile.h"
#include "DFGCommonData.h"
#include "FTLLazySlowPath.h"
#include "FTLOSRExit.h"
#include "JITCode.h"
#include "JITOpaqueByproducts.h"

namespace JSC {

class TrackedReferences;

namespace FTL {

// Handler ICs (data ICs) compiled by InlineCacheCompiler read the per-tier JIT
// data through GPRInfo::jitDataRegister, using BaselineJITData's field offsets
// (see e.g. the post-call stack-pointer restoration in InlineCacheCompiler.cpp,
// which loads [jitDataRegister + BaselineJITData::offsetOfStackOffset()]).
// Baseline and DFG pin jitDataRegister for the whole function; FTL does not, so
// each FTL handler-IC site materializes a pointer to this structure into
// jitDataRegister right before dispatching into the handler chain. The leading
// field layout therefore MUST mirror BaselineJITData/DFG::JITData (static_asserts
// in FTLJITCode.cpp).
class JITData {
    WTF_MAKE_NONCOPYABLE(JITData);
public:
    JITData() = default;

    static constexpr ptrdiff_t offsetOfGlobalObject() { return OBJECT_OFFSETOF(JITData, m_globalObject); }
    static constexpr ptrdiff_t offsetOfStackOffset() { return OBJECT_OFFSETOF(JITData, m_stackOffset); }
    static constexpr ptrdiff_t offsetOfDummyArrayProfile() { return OBJECT_OFFSETOF(JITData, m_dummyArrayProfile); }

    // BaselineJITData and DFG::JITData derive from ButterflyArray, whose header
    // (m_leadingSize/m_trailingSize) precedes their members. Handler ICs read
    // through jitDataRegister using BaselineJITData's field offsets regardless
    // of tier, so mirror that header here to keep the offsets equal (enforced
    // by the static_asserts in FTLJITCode.cpp). FTL needs no leading/trailing
    // arrays, so the fields are unused.
    unsigned m_unusedButterflyArrayLeadingSize { 0 };
    unsigned m_unusedButterflyArrayTrailingSize { 0 };
    JSGlobalObject* m_globalObject { nullptr }; // This is not marked since the owner CodeBlock will mark JSGlobalObject.
    intptr_t m_stackOffset { 0 };
    // The shared by-val slow-path handler thunks pass profileGPR to the optimize
    // operations, so by-val handler-IC sites must point it at a real ArrayProfile.
    // Like the DFG (DFG::JITData::m_dummyArrayProfile), the FTL has no per-site
    // profile to feed, so all sites share this dummy.
    ArrayProfile m_dummyArrayProfile { };
};

class JITCode : public JSC::JITCode {
public:
    JITCode();
    ~JITCode() override;

    CodePtr<JSEntryPtrTag> addressForCall(ArityCheckMode) override;
    void* executableAddressAtOffset(size_t offset) override;
    void* dataAddressAtOffset(size_t offset) override;
    unsigned offsetOf(void* pointerIntoCode) override;
    size_t size() override;
    void setSize(size_t size) { m_size = size; }
    bool contains(void*) override;

    void initializeB3Code(CodeRef<JSEntryPtrTag>);
    void initializeB3Byproducts(std::unique_ptr<OpaqueByproducts>);
    void NODELETE initializeAddressForCall(CodePtr<JSEntryPtrTag>);
    void NODELETE initializeAddressForArityCheck(CodePtr<JSEntryPtrTag>);
    
    void validateReferences(const TrackedReferences&) override;

    RegisterSet liveRegistersToPreserveAtExceptionHandlingCallSite(CodeBlock*, CallSiteIndex) override;

    std::optional<CodeOrigin> findPC(CodeBlock*, void* pc) override;

    CodeRef<JSEntryPtrTag> b3Code() const { return m_b3Code; }
    
    JITCode* ftl() override;
    DFG::CommonData* dfgCommon() override;
    const DFG::CommonData* dfgCommon() const override;
    static constexpr ptrdiff_t commonDataOffset() { return OBJECT_OFFSETOF(JITCode, common); }
    void shrinkToFit() override;

    bool isUnlinked() const { return common.isUnlinked(); }

    PCToCodeOriginMap* pcToCodeOriginMap() override { return common.m_pcToCodeOriginMap.get(); }

    const RegisterAtOffsetList* calleeSaveRegisters() const LIFETIME_BOUND { return &m_calleeSaveRegisters; }

    unsigned numberOfCompiledDFGNodes() const { return m_numberOfCompiledDFGNodes; }
    void setNumberOfCompiledDFGNodes(unsigned numberOfCompiledDFGNodes)
    {
        m_numberOfCompiledDFGNodes = numberOfCompiledDFGNodes;
    }

    // Handler-IC support (useHandlerICInFTL): see the JITData comment above. The
    // address is baked into the compiled code as a constant at each handler-IC
    // site, so this object must live exactly as long as this JITCode (it does:
    // it is an inline member). It is filled in at plan finalization, before the
    // code is installed, by FTL::JITFinalizer::finalize().
    JITData* handlerICJITData() { return &m_handlerICJITData; }
    ArrayProfile* handlerICDummyArrayProfile() { return &m_handlerICJITData.m_dummyArrayProfile; }
    void initializeHandlerICJITData(JSGlobalObject* globalObject, intptr_t stackOffset)
    {
        m_handlerICJITData.m_globalObject = globalObject;
        m_handlerICJITData.m_stackOffset = stackOffset;
    }
    void finalizeHandlerICDataUnconditionally()
    {
        // Mirrors DFG::JITData::finalizeUnconditionally(): drop potentially
        // dead StructureIDs accumulated in the dummy profile.
        m_handlerICJITData.m_dummyArrayProfile.clear();
    }

    DFG::CommonData common;
    Vector<OSRExit> m_osrExit;
    RegisterAtOffsetList m_calleeSaveRegisters;
    SegmentedVector<OSRExitDescriptor, 8> osrExitDescriptors;
    Vector<std::unique_ptr<LazySlowPath>> lazySlowPaths;
    
private:
    CodeRef<JSEntryPtrTag> m_b3Code;
    std::unique_ptr<OpaqueByproducts> m_b3Byproducts;
    CodePtr<JSEntryPtrTag> m_addressForArityCheck;
    JITData m_handlerICJITData;
    size_t m_size { 1000 };
    unsigned m_numberOfCompiledDFGNodes { 0 };
};

} } // namespace JSC::FTL

#endif // ENABLE(FTL_JIT)
