/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "B3HeapRange.h"
#include "B3Origin.h"
#include "B3Policy.h"
#include "JSExportMacros.h"
#include <optional>
#include <wtf/BitVector.h>
#include <wtf/Vector.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT {

enum class NarrowReferenceViolationKind : uint8_t {
    // Nothing but the 32-bit reference names the cell while the collector can run.
    HasNoPointer,
    // As above, and the live value is the reference plus or minus a constant, which no scan of the stack recognizes.
    IsDisplaced,
};

struct NarrowReferenceViolation {
    NarrowReferenceViolationKind kind;
    B3::Origin reference;
    B3::Origin safepoint;
    String detail;
};

struct NarrowReferenceFindings {
    Vector<NarrowReferenceViolation> violations;
    uint32_t numberOfReferencesAcrossSafepoints { 0 };
    // Stores into objects that lie in the frame. No pointer stands beside what they store: the scan of the stack alone finds it.
    uint32_t numberOfReferencesStoredInFrame { 0 };
};

// A value that may collect writes the first two and reads the third. A word loaded from the last is a root for as long as the program runs.
struct HeapsOfCollection {
    B3::HeapRange collectorState;
    B3::HeapRange weakTableStorage;
    B3::HeapRange frameSlots;
    B3::HeapRange permanentCells;
};

// By the index of the operation that is the origin of a B3 value.
struct NarrowReferenceOrigins {
    // A 32-bit load, a 32-bit Phi or a shift of a pointer with one of these origins is a narrow reference, whether or not the function decodes it.
    BitVector references;
    BitVector suspensions;
    // More such origins, found while the procedure is made.
    const BitVector* laterReferences { nullptr };
    // For the test of what notices a missing guard: of the references with these origins, the one with this ordinal among those that would get a pointer gets none.
    BitVector originsOfDroppedGuard { };
    std::optional<uint32_t> ordinalOfDroppedGuard { };
};

JS_EXPORT_PRIVATE void findNarrowReferenceViolations(B3::Procedure&, const HeapsOfCollection&, const NarrowReferenceOrigins&, NarrowReferenceFindings&);
JS_EXPORT_PRIVATE B3::ClientPhase validateNarrowReferences(NarrowReferenceFindings&, NarrowReferenceOrigins&&);

// Puts a pointer beside every narrow reference that would be found without one, and returns how many. It runs behind the optimizations: they share and
// hoist loads of references over calls that the operations of the program did not read them over, and sink the sum that decodes one below a call.
JS_EXPORT_PRIVATE uint32_t insertNarrowReferenceGuards(B3::Procedure&, const HeapsOfCollection&, const NarrowReferenceOrigins&);
JS_EXPORT_PRIVATE B3::ClientPhase guardNarrowReferences(uint32_t& numberOfGuards, NarrowReferenceOrigins&&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
