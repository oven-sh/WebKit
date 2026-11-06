/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTLedger.h"
#include "AOTThreads.h"
#include <span>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

// Who writes the fields of the layouts of objects. A field is named by its declaration: what the objects of every layout that holds it get is listed there.
// A value that an allocation lists has a write of its own, with the allocation as its site. What the other writers hand over is listed once for each actor and form.
class LedgerOfFields final : public LedgerPart {
    WTF_MAKE_NONCOPYABLE(LedgerOfFields);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(LedgerOfFields);
public:
    struct NumberedWrite {
        uint32_t field;
        Write write;
    };
    using Notes = Vector<NumberedWrite>;

    explicit LedgerOfFields(const Program&);

    uint32_t numberOf(FieldReference declaration) const { return m_firstFieldOfLayout[declaration.layout.index()] + declaration.field; }

    // The objects of one layout that holds the field: who has not accounted for them, and what was seen to be written. Several threads sign at once, each for fields of its own.
    void sign(Notes&, FieldReference declaration, OptionSet<Actor> open, std::span<const Write>);
    void note(Notes& notes, FieldReference declaration, const Write& write) const { notes.append({ numberOf(declaration), write }); }
    // The writes to a field are listed in the order of the lists, and as they stand in each.
    void close(const ParallelLoops&, Vector<Notes>&&);

    std::optional<Writers> writersOf(const Place&) const final;
    Writers writersOf(FieldReference declaration) const;

private:
    const Program& m_program;
    Vector<uint32_t> m_firstFieldOfLayout;
    // A field that nobody has signed for is open to everybody who can write one.
    Vector<OptionSet<Actor>> m_open;
    Vector<bool> m_isSignedFor;
    ParallelArray<Write> m_writes;
    Vector<uint32_t> m_firstWriteOfField;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
