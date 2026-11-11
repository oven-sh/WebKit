/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCodeRecords.h"
#include "AOTOperationEmitter.h"
#include "AOTVector.h"
#include "RegisterSet.h"
#include <optional>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT {

class StubCatalog;

namespace Typed {

class ThrowEmitter final : public OperationEmitter {
public:
    explicit ThrowEmitter(CodeGenerator&);

    void throwAt(const OperationContext&, const ThrowSite&, std::optional<Operand>);
    void requireOrThrow(const OperationContext&, Condition mustHold, const ThrowSite&, std::optional<Operand>);
    void throwError(const OperationContext&, ErrorKind, StringID message, std::optional<uint32_t> throwPosition);
    void crashOnFalseProof(const OperationContext&, std::optional<Boxed> found, ProofSource, uint32_t expectedType = 0);
    void checkOrCrash(const OperationContext&, Condition holds, ProofSource, uint32_t expectedType = 0);
    void requireOrCrash(const OperationContext&, Condition holds, std::optional<Boxed> found, ProofSource, uint32_t expectedType = 0);
    void stopUnlessStubPasses(const OperationContext&, Boxed found, StubKey, uint32_t kinds, ProofSource, std::optional<uint32_t> mentionAboveKinds = std::nullopt, std::optional<uint32_t> numberInRegister = std::nullopt);

    void requireNotNullish(const OperationContext&, Operand, const Type&, NullishAccess, StringID name);
    void requireInitialized(const OperationContext&, Operand, const Type&, StringID name);

private:
    struct NewError {
        ErrorKind kind;
        std::optional<uint32_t> throwPosition;
    };

    void throwAt(const OperationContext&, const ThrowSite&, std::optional<Operand>, std::optional<NewError>);
    Operand inFormNamedByThrowWords(const OperationContext&, Operand);
    Machine::PatchGenerator* generatorOfCheckedThrowSite(const OperationContext&, const ThrowSite&, Representation);
    bool canTrapAt(const OperationContext&);
    void trapThrough(const OperationContext&, LValue pointer, Representation, const ThrowSite&);

    struct CheckedThrowSite {
        ThrowSite site;
        Representation representation;
        RecordedSite position;
        Machine::PatchGenerator* generator;
    };

    const RegisterSet m_clobberedRegisters;
    UncheckedKeyHashMap<uint64_t, Vector<CheckedThrowSite, 1>, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_checkedThrowSites;
};

void emitStraightThrowSite(CCallHelpers&, CodeRecords&, const ThrowSite&, std::optional<GPRReg> operand, unsigned registerNumberWithoutOperand, uint32_t callSite);
Vector<uint32_t, 3> throwSiteWords(const ThrowSite&, Representation, unsigned registerNumber);
inline uint8_t mentionsAmongThrowSiteWords(const ThrowSite& site, size_t numberOfWords)
{
    uint8_t bits = 0;
    if (site.aIsMention)
        bits |= 1 << 1;
    if (site.b && site.bIsMention)
        bits |= 1 << (numberOfWords - 1);
    return bits;
}
RegisterSet registersClobberedByThrowSites(const StubCatalog&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
