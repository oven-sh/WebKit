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
    void throwError(const OperationContext&, ErrorKind, StringID message);

    void requireNotNullish(const OperationContext&, Operand, const Type&, NullishAccess, StringID name);
    void requireInitialized(const OperationContext&, Operand, const Type&, StringID name);

private:
    void emitThrowSite(const OperationContext&, const ThrowSite&, std::optional<Operand>, std::optional<ErrorKind> = std::nullopt);
    void checkOrThrow(const OperationContext&, Condition mustHold, const ThrowSite&, std::optional<Operand>);
    RefPtr<B3::StackmapGenerator> generatorOfCheckedThrowSite(const OperationContext&, const ThrowSite&, Representation);
    void branchOrThrow(const OperationContext&, Condition mustHold, const ThrowSite&, std::optional<Operand>);
    bool canTrapAt(const OperationContext&);
    void trapIfNullish(const OperationContext&, Boxed, const ThrowSite&);
    void trapIfNullish(const OperationContext&, NarrowReference, const ThrowSite&);
    void trapThrough(const OperationContext&, LValue pointer, Representation, const ThrowSite&);
    static ThrowKind throwKindOf(NullishAccess);

    struct CheckedThrowSite {
        ThrowSite site;
        Representation representation;
        RecordedSite position;
        RefPtr<B3::StackmapGenerator> generator;
    };

    const RegisterSet m_clobberedRegisters;
    UncheckedKeyHashMap<unsigned, Vector<CheckedThrowSite, 1>, IntHash<unsigned>, WTF::UnsignedWithZeroKeyHashTraits<unsigned>> m_checkedThrowSites;
};

Vector<uint32_t, 3> throwSiteWords(const ThrowSite&, Representation, unsigned registerNumber);
RegisterSet registersClobberedByThrowSites(const StubCatalog&);
B3::ClientPhase throwWhereTrapsAreCertain(CodeRecords&, const StubCatalog&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
