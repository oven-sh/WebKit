/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
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
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include <array>
#include <wtf/FastMalloc.h>
#include <wtf/StdLibExtras.h>

#if CPU(X86_64)
#include <x86intrin.h>
#endif

namespace JSC {

class CodeBlock;
class FunctionExecutable;
class UnlinkedCodeBlock;
class UnlinkedFunctionExecutable;

// Options::reportCodeBlockCreationCosts(): cheap counters for where per-function creation time goes
// (bytecode-cache decode of UnlinkedCodeBlocks and CodeBlock linking). Measurement aid, not hardened: the "current
// decode/link" cursors are per thread and the totals are shared by every VM in the process, cells are identified by
// address, and a dump (periodic or at exit) reads other threads' in-flight records and their live CodeBlocks (execute
// counters, JIT type, identifiers) without synchronizing with those VMs. Intended for exit-time dumps; periodic dumps
// are meaningful with a single VM only.
namespace CodeBlockCreationStats {

#define FOR_EACH_CODEBLOCK_CREATION_BUCKET(v) \
    v(DecodeIntegrityCheck, "decode: region integrity/checksum") \
    v(DecodeFixed, "decode: cell alloc + scalars + metadata table + instruction stream + rare data") \
    v(DecodeConstants, "decode: constant pool (incl. SymbolTables, strings, etc.)") \
    v(DecodeConstantSymbolTables, "decode:   of which SymbolTable constants") \
    v(DecodeMisc, "decode: source-code-representation + expression info + jump targets") \
    v(DecodeIdentifiers, "decode: identifier table (atomize)") \
    v(DecodeChildren, "decode: child UnlinkedFunctionExecutables (decls+exprs)") \
    v(DecodeChildName, "decode:   of which child ecmaName atomize") \
    v(DecodeChildTDZ, "decode:   of which child parentScopeTDZVariables") \
    v(DecodeChildRareData, "decode:   of which child rare data") \
    v(DecodeChildNameLazy, "decode: child ecmaName atomized on first use (useThinChildExecutables; count = executables)") \
    v(DecodeChildMembersLazy, "decode: child TDZ variables + rare data decoded on first use (useThinChildExecutables; count = executables)") \
    v(DecodeOwnMembers, "decode: derived-class members (program/module var declarations etc.)") \
    v(ExternalAtomSlotHit, "decode:   count only: DecoderStringTable::atomFor answered from its slot (string already atomized on this thread; folded in per decoded block)") \
    v(ExternalAtomPromoted, "decode:   count only: DecoderStringTable::atomFor promoted the plain string a constant left in the slot (atom-table insert)") \
    v(ExternalAtomCreated, "decode:   count only: DecoderStringTable::atomFor atomized a table string for the first time on a thread (atom-table insert)") \
    v(AtomTableGrew, "decode:   count only: decoded blocks after which the thread's atom string table had more buckets than at the previous check (each transition is also logged)") \
    v(AtomTableShrank, "decode:   count only: ... fewer buckets (a removal shrank it below 1/6 load; the regrowth that follows is rehash work reserveCapacityForCurrentThread caused)") \
    v(DecodeTotal, "decode: TOTAL per UnlinkedCodeBlock (inclusive)") \
    v(DecodeFunctionCodeBlockLazy, "decode: lazy UnlinkedFunctionCodeBlock decode calls (inclusive, subset of DecodeTotal)") \
    v(LinkMetadataCreate, "link: CodeBlock ctor incl. MetadataTable allocation") \
    v(LinkConstants, "link: constant registers (SymbolTable clone, Set constants)") \
    v(LinkConstantSymbolTableClone, "link:   of which SymbolTable cloneScopePart") \
    v(MaterializeSymbolTableEntriesLazy, "lazy: SymbolTable entry decode on first read (useLazySymbolTableConstants; count = tables; also inside whichever decode/link lap is open)") \
    v(LinkFunctions, "link: function decl/expr FunctionExecutable creation") \
    v(LinkFunctionsLazy, "lazy: FunctionExecutable created on first new_func* / before JIT (useLazyFunctionExecutables; count = executables)") \
    v(DecodeChildScalarsLazy, "lazy: child cold scalars decoded on first call / introspection (useThinChildExecutables; count = executables)") \
    v(LinkHandlers, "link: exception handlers") \
    v(LinkInstructionWalk, "link: instruction-stream walk (metadata init, scope resolution)") \
    v(LinkScopeResolution, "link:   of which resolve_scope/get_from_scope/put_to_scope cases (abstractResolve)") \
    v(LinkProfiledOpcodeMetadata, "link:   count only: plain LINK() metadata entries placement-new'd (get_by_id, call, ...); time = walk - scope") \
    v(LinkTemplateObjects, "link: template objects + tail") \
    v(LinkTotal, "link: TOTAL CodeBlock::CodeBlock+finishCreation (inclusive)") \
    v(JITPrepareLazyState, "jit: prepareLazyStateForConcurrentCompilation slow path (mutator, before a JIT tier / replacement / inlining)") \
    v(JITPrepareInlineCandidates, "jit: DFG::compile inline-candidate pre-walk (mutator, inclusive of the above)") \
    v(JITInlineeRefusedUnprepared, "jit:   count only: DFG inlinees refused because their lazy state was not prepared") \

enum class Bucket : uint8_t {
#define DECLARE_BUCKET(name, desc) name,
    FOR_EACH_CODEBLOCK_CREATION_BUCKET(DECLARE_BUCKET)
#undef DECLARE_BUCKET
    NumberOfBuckets
};
static constexpr unsigned numberOfBuckets = static_cast<unsigned>(Bucket::NumberOfBuckets);

extern JS_EXPORT_PRIVATE bool g_enabled;
JS_EXPORT_PRIVATE void initialize(); // reads Options, installs exit hooks

ALWAYS_INLINE bool enabled() { return g_enabled; }

ALWAYS_INLINE uint64_t now()
{
#if CPU(X86_64)
    return __rdtsc();
#else
    return nowSlow();
#endif
}
JS_EXPORT_PRIVATE uint64_t nowSlow();

JS_EXPORT_PRIVATE void add(Bucket, uint64_t ticks, uint64_t count = 1);

struct Scope {
    explicit Scope(Bucket bucket)
        : m_bucket(bucket)
        , m_start(enabled() ? now() : 0)
    {
    }
    ~Scope()
    {
        if (m_start)
            add(m_bucket, now() - m_start);
    }
    uint64_t elapsed() const { return m_start ? now() - m_start : 0; }
    Bucket m_bucket;
    uint64_t m_start;
};

// Per decoded UnlinkedCodeBlock.
struct DecodeRecord {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(DecodeRecord);
    UnlinkedCodeBlock* codeBlock { nullptr };
    std::array<uint64_t, numberOfBuckets> ticks { };
    unsigned instructionBytes { 0 };
    unsigned identifiers { 0 };
    unsigned children { 0 };
    unsigned childrenWithName { 0 };
    unsigned childrenWithTDZ { 0 };
    unsigned constants { 0 };
    unsigned symbolTableConstants { 0 };
    unsigned symbolTableEntries { 0 };
    unsigned stringConstants { 0 };
    unsigned stringConstantCellsCreated { 0 }; // DecoderStringTable::jsStringFor made a new cell (the rest were table hits, SmallStrings or inline)
    unsigned handlers { 0 };
    bool isFunctionCode { false };
    // filled when the UnlinkedCodeBlock dies, or at dump time
    unsigned identifiersTouched { 0 };
    bool dead { false };
    bool everLinked { false };
};

// While decoding a block, nested helper decodes (children, constants) charge this record. Per thread.
JS_EXPORT_PRIVATE DecodeRecord* beginDecode();
JS_EXPORT_PRIVATE void endDecode(DecodeRecord*, DecodeRecord* previous, UnlinkedCodeBlock*);
JS_EXPORT_PRIVATE DecodeRecord* currentDecode();
JS_EXPORT_PRIVATE void setCurrentDecode(DecodeRecord*);
ALWAYS_INLINE void addToCurrent(Bucket bucket, uint64_t ticks, uint64_t count = 1)
{
    add(bucket, ticks, count);
    if (DecodeRecord* r = currentDecode())
        r->ticks[static_cast<unsigned>(bucket)] += ticks;
}

JS_EXPORT_PRIVATE void noteChildExecutableDecoded(UnlinkedFunctionExecutable*, bool hasName, bool hasTDZ, bool hasRareData);
JS_EXPORT_PRIVATE void noteExecutableLinked(UnlinkedFunctionExecutable*);
JS_EXPORT_PRIVATE void noteFunctionInstantiated(FunctionExecutable*);
JS_EXPORT_PRIVATE void noteUnlinkedCodeBlockDestroyed(UnlinkedCodeBlock*);
JS_EXPORT_PRIVATE void noteIdentifierTableCreated(UnlinkedCodeBlock*, unsigned count);

// Per linked CodeBlock.
struct LinkRecord {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(LinkRecord);
    CodeBlock* codeBlock { nullptr };
    UnlinkedCodeBlock* unlinkedCodeBlock { nullptr };
    std::array<uint64_t, numberOfBuckets> ticks { };
    unsigned instructionBytes { 0 };
    unsigned instructionCount { 0 };
    unsigned profiledMetadataOps { 0 };
    unsigned metadataBytes { 0 };
    unsigned identifiers { 0 };
    unsigned children { 0 };
    unsigned constants { 0 };
    unsigned symbolTablesCloned { 0 };
    unsigned handlers { 0 };
    double initialExecuteCount { 0 };
    uint8_t codeType { 0 };
    // filled when the CodeBlock dies, or at dump time
    double finalExecuteCount { 0 };
    uint8_t finalJITType { 0 };
    bool dead { false };
};
JS_EXPORT_PRIVATE LinkRecord* beginLink(CodeBlock*, UnlinkedCodeBlock*);
JS_EXPORT_PRIVATE void endLink(LinkRecord*);
JS_EXPORT_PRIVATE void noteCodeBlockDestroyed(CodeBlock*);

JS_EXPORT_PRIVATE void dump(const char* reason);
JS_EXPORT_PRIVATE void maybePeriodicDump();

} // namespace CodeBlockCreationStats

} // namespace JSC
