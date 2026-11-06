/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCodeRecords.h"
#include "AOTOperationEmitter.h"
#include "AOTVector.h"
#include "B3Policy.h"
#include "B3StackmapValue.h"
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
    // The stack of the Error begins at the position of the context.
    void throwError(const OperationContext&, ErrorKind, StringID message, std::optional<uint32_t> throwPosition);

    void requireNotNullish(const OperationContext&, Operand, const Type&, NullishAccess, StringID name);
    void requireInitialized(const OperationContext&, Operand, const Type&, StringID name);

private:
    struct NewError {
        ErrorKind kind;
        std::optional<uint32_t> throwPosition;
    };

    void throwAt(const OperationContext&, const ThrowSite&, std::optional<Operand>, std::optional<NewError>);
    Operand inFormNamedByThrowWords(const OperationContext&, Operand);
    RefPtr<B3::StackmapGenerator> generatorOfCheckedThrowSite(const OperationContext&, const ThrowSite&, Representation);
    bool canTrapAt(const OperationContext&);
    void trapThrough(const OperationContext&, LValue pointer, Representation, const ThrowSite&);

    struct CheckedThrowSite {
        ThrowSite site;
        Representation representation;
        RecordedSite position;
        RefPtr<B3::StackmapGenerator> generator;
    };

    const RegisterSet m_clobberedRegisters;
    UncheckedKeyHashMap<uint64_t, Vector<CheckedThrowSite, 1>, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_checkedThrowSites;
};

Vector<uint32_t, 3> throwSiteWords(const ThrowSite&, Representation, unsigned registerNumber);
RegisterSet registersClobberedByThrowSites(const StubCatalog&);
B3::ClientPhase throwWhereTrapsAreCertain(CodeRecords&, const StubCatalog&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
