/*
 * Copyright (C) 2015-2023 Apple Inc. All rights reserved.
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

#if ENABLE(B3_JIT)

#include "B3AbstractHeapRepository.h"
#include "B3Arena.h"
#include "B3CompilationReport.h"
#include "B3HeapSet.h"
#include "B3InductionRecords.h"
#include "B3KnownProperties.h"
#include "B3MovedLoadCheck.h"
#include "B3Origin.h"
#include "B3PCToOriginMap.h"
#include "B3Policy.h"
#include "B3SparseCollection.h"
#include "B3Type.h"
#include "B3ValueKey.h"
#include "JITOpaqueByproducts.h"
#include "PureNaN.h"
#include "RegisterAtOffsetList.h"
#include <wtf/Bag.h>
#include <wtf/BitSet.h>
#include <wtf/FastMalloc.h>
#include <wtf/HashSet.h>
#include <wtf/IndexedContainerIterator.h>
#include <wtf/JSONValues.h>
#include <wtf/Noncopyable.h>
#include <wtf/PrintStream.h>
#include <wtf/SequesteredMalloc.h>
#include <wtf/SharedTask.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/TriState.h>
#include <wtf/Vector.h>
#include <wtf/text/WTFString.h>

namespace JSC {

class CCallHelpers;

namespace B3 {

class BackwardsCFG;
class BackwardsDominators;
class BasicBlock;
class BlockInsertionSet;
class BlockRarity;
class CFG;
class CompilationScratch;
class ConstPtrValue;
class Dominators;
struct ValidatedForm;
class FrequencyEstimate;
class MemoryValue;
class NaturalLoops;
class Value;
class Variable;
class WasmBoundsCheckValue;

namespace Air {
class Code;
class StackSlot;
} // namespace Air

typedef void WasmBoundsCheckGeneratorFunction(CCallHelpers&, WasmBoundsCheckValue*, GPRReg);
typedef SharedTask<WasmBoundsCheckGeneratorFunction> WasmBoundsCheckGenerator;

// This represents B3's view of a piece of code. Note that this object must exist in a 1:1
// relationship with Air::Code. B3::Procedure and Air::Code are just different facades of the B3
// compiler's knowledge about a piece of code. Some kinds of state aren't perfect fits for either
// Procedure or Code, and are placed in one or the other based on convenience. Procedure always
// allocates a Code, and a Code cannot be allocated without an owning Procedure and they always
// have references to each other.

class Procedure {
    WTF_MAKE_NONCOPYABLE(Procedure);
    WTF_MAKE_SEQUESTERED_ARENA_ALLOCATED(Procedure);
public:

    JS_EXPORT_PRIVATE Procedure(bool usesSIMD = false);
    JS_EXPORT_PRIVATE explicit Procedure(const Policy&, CompilationScratch* = nullptr, bool usesSIMD = false);
    JS_EXPORT_PRIVATE ~Procedure();

    template<typename Callback>
    void setOriginPrinter(Callback&& callback)
    {
        m_originPrinter = createSharedTask<void(PrintStream&, Origin)>(
            std::forward<Callback>(callback));
    }

    // Usually you use this via OriginDump, though it's cool to use it directly.
    void printOrigin(PrintStream& out, Origin origin) const;

    void setName(String name) { m_name = WTF::move(name); }
    const String& name() const { return m_name; }

    // This is a debugging hack. Sometimes while debugging B3 you need to break the abstraction
    // and get at the DFG Graph, or whatever data structure the frontend used to describe the
    // program. The FTL passes the DFG Graph.
    void setFrontendData(const void* value) { m_frontendData = value; }
    const void* frontendData() const { return m_frontendData; }

    // A number by which the client knows what it compiles here. Nothing in B3 depends on it.
    void setSubjectOfClient(uint32_t subject) { m_subjectOfClient = subject; }
    uint32_t subjectOfClient() const { return m_subjectOfClient; }

    JS_EXPORT_PRIVATE BasicBlock* addBlock(double frequency = 1);

    // Changes the order of basic blocks to be as in the supplied vector. The vector does not
    // need to mention every block in the procedure. Blocks not mentioned will be placed after
    // these blocks in the same order as they were in originally.
    template<typename BlockIterable>
    void setBlockOrder(const BlockIterable& iterable)
    {
        Vector<BasicBlock*> blocks;
        for (BasicBlock* block : iterable)
            blocks.append(block);
        setBlockOrderImpl(blocks);
    }

    JS_EXPORT_PRIVATE Air::StackSlot* addStackSlot(uint64_t byteSize);
    JS_EXPORT_PRIVATE Variable* addVariable(Type);

    JS_EXPORT_PRIVATE Type addTuple(Vector<Type>&& types);
    const Vector<Vector<Type>>& tuples() const { return m_tuples; };
    bool LEGACY_NODELETE isValidTuple(Type tuple) const;
    Type extractFromTuple(Type tuple, unsigned index) const;
    JS_EXPORT_PRIVATE const Vector<Type>& LEGACY_NODELETE tupleForType(Type tuple) const;

    unsigned resultCount(Type type) const { return type.isTuple() ? tupleForType(type).size() : type.isNumeric(); }
    Type typeAtOffset(Type type, unsigned index) const { ASSERT(index < resultCount(type)); return type.isTuple() ? extractFromTuple(type, index) : type; }

    template<typename ValueType, typename... Arguments>
    ValueType* add(Arguments...);

    JS_EXPORT_PRIVATE Value* clone(Value*);

    Value* addIntConstant(Origin, Type, int64_t value);
    Value* addIntConstant(Value*, int64_t value);

    // bits is a std::bit_cast of the constant you want.
    JS_EXPORT_PRIVATE Value* addConstant(Origin, Type, uint64_t bits);
    JS_EXPORT_PRIVATE Value* addConstant(Origin, Type, v128_t bits);

    // You're guaranteed that bottom is zero.
    Value* addBottom(Origin, Type);
    Value* addBottom(Value*);

    // Returns null for TriState::Indeterminate.
    Value* addBoolConstant(Origin, TriState);

    JS_EXPORT_PRIVATE void LEGACY_NODELETE resetValueOwners();
    JS_EXPORT_PRIVATE void resetReachability();

    // This destroys CFG analyses. If we ask for them again, we will recompute them. Usually you
    // should call this anytime you call resetReachability().
    JS_EXPORT_PRIVATE void invalidateCFG();

    std::unique_ptr<ValidatedForm>& validatedForm() LIFETIME_BOUND { return m_validatedForm; }

    JS_EXPORT_PRIVATE void dump(PrintStream&) const;

    unsigned size() const { return m_blocks.size(); }
    BasicBlock* at(unsigned index) const { return m_blocks[index].get(); }
    BasicBlock* operator[](unsigned index) const { return at(index); }

    typedef WTF::IndexedContainerIterator<Procedure> iterator;

    iterator begin() const { return iterator(*this, 0); }
    iterator end() const { return iterator(*this, size()); }

    JS_EXPORT_PRIVATE Vector<BasicBlock*> blocksInPreOrder();
    JS_EXPORT_PRIVATE Vector<BasicBlock*> blocksInPostOrder();
    JS_EXPORT_PRIVATE const Vector<BasicBlock*>& cachedBlocksInPreOrder() LIFETIME_BOUND;
    JS_EXPORT_PRIVATE const Vector<BasicBlock*>& cachedBlocksInPostOrder() LIFETIME_BOUND;
    JS_EXPORT_PRIVATE bool hasCycle();

    // The cached orders follow the order of a block's successors, which no other analysis of the CFG does.
    JS_EXPORT_PRIVATE void didReorderSuccessors();
    // The frequencies of blocks are sums that are added up in the order of the successors.
    JS_EXPORT_PRIVATE void willReorderSuccessors();

    SparseCollection<Air::StackSlot>& stackSlots();
    const SparseCollection<Air::StackSlot>& stackSlots() const;

    SparseCollection<Variable>& variables() LIFETIME_BOUND { return m_variables; }
    const SparseCollection<Variable>& variables() const LIFETIME_BOUND { return m_variables; }

    // Short for variables().remove(). It's better to call this method since it's out of line.
    void deleteVariable(Variable*);
    void deleteAllVariables();

    SparseCollection<Value>& values() LIFETIME_BOUND { return m_values; }
    const SparseCollection<Value>& values() const LIFETIME_BOUND { return m_values; }

    // Short for values().remove(). It's better to call this method since it's out of line.
    JS_EXPORT_PRIVATE void deleteValue(Value*);

    JS_EXPORT_PRIVATE void setKnownRange(Value*, IntRange);
    std::optional<IntRange> knownRange(const Value* value) const { return m_knownProperties.range(value); }

    JS_EXPORT_PRIVATE void setKnownDereferenceableBytes(Value*, uint32_t);
    uint32_t knownDereferenceableBytes(const Value* value) const { return m_knownProperties.dereferenceableBytes(value); }
    JS_EXPORT_PRIVATE bool isKnownDereferenceable(const MemoryValue*) const;
    JS_EXPORT_PRIVATE bool isKnownDereferenceable(const Value* address, int64_t offset, size_t byteSize) const;
    JS_EXPORT_PRIVATE Effects effectsOf(const Value*) const;
    KnownProperties& knownProperties() LIFETIME_BOUND { return m_knownProperties; }

    JS_EXPORT_PRIVATE void setKnownInduction(const InductionRecord&);
    JS_EXPORT_PRIVATE void setKnownInduction(const DemotedInduction&);
    std::optional<InductionRecord> knownInduction(const Value* phi) const { return m_knownInductions.find(phi); }
    const InductionRecords& knownInductions() const LIFETIME_BOUND { return m_knownInductions; }
    InductionRecords& knownInductions() LIFETIME_BOUND { return m_knownInductions; }

    MovedLoadLog& movedLoadLog() LIFETIME_BOUND { return m_movedLoadLog; }

    // A valid procedure cannot contain any orphan values. An orphan is a value that is not in
    // any basic block. It is possible to create an orphan value during code generation or during
    // transformation. If you know that you may have created some, you can call this method to
    // delete them, making the procedure valid again.
    void deleteOrphans();

    CFG& cfg() const { return *m_cfg; }

    JS_EXPORT_PRIVATE Dominators& dominators();
    JS_EXPORT_PRIVATE NaturalLoops& naturalLoops();
    BackwardsCFG& backwardsCFG();
    JS_EXPORT_PRIVATE BackwardsDominators& backwardsDominators();
    JS_EXPORT_PRIVATE BlockRarity& blockRarity();
    JS_EXPORT_PRIVATE void updateEdgeClasses();
    JS_EXPORT_PRIVATE void updateFrequencies();

    void addFastConstant(const ValueKey&);
    bool LEGACY_NODELETE isFastConstant(const ValueKey&);
    
    unsigned numEntrypoints() const { return m_numEntrypoints; }
    JS_EXPORT_PRIVATE void setNumEntrypoints(unsigned);

    // The name has to be a string literal, since we don't do any memory management for the string.
    void setLastPhaseName(const char* name)
    {
        m_lastPhaseName = name;
    }

    const char* lastPhaseName() const { return m_lastPhaseName; }

    // Allocates a slab of memory that will be kept alive by anyone who keeps the resulting code
    // alive. Great for compiler-generated data sections, like switch jump tables and constant pools.
    // This returns memory that has been zero-initialized.
    JS_EXPORT_PRIVATE void* addDataSection(size_t);

    struct DataSectionRecord {
        void* data;
        size_t size;
    };

    JS_EXPORT_PRIVATE Value* addDataSectionAddress(Origin, const void* dataSection);
    std::span<const DataSectionRecord> dataSections() const LIFETIME_BOUND { return m_dataSections.span(); }

    struct JumpTableRecord {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(JumpTableRecord);
        Vector<uint32_t> targetOffsets;
    };

    JS_EXPORT_PRIVATE unsigned addJumpTable(unsigned numberOfEntries);
    JS_EXPORT_PRIVATE Value* addJumpTableAddress(Origin, unsigned jumpTable);
    JumpTableRecord& jumpTable(unsigned index) LIFETIME_BOUND { return *m_jumpTables[index]; }
    unsigned numberOfJumpTables() const { return m_jumpTables.size(); }

    // Some operations are specified in B3 IR to behave one way but on this given CPU they behave a
    // different way. When true, those B3 IR ops switch to behaving the CPU way, and the optimizer may
    // start taking advantage of it.
    //
    // One way to think of it is like this. Imagine that you find that the cleanest way of lowering
    // something in lowerMacros is to unconditionally replace one opcode with another. This is a shortcut
    // where you instead keep the same opcode, but rely on the opcode's meaning changes once lowerMacros
    // sets hasQuirks.
    bool hasQuirks() const { return m_hasQuirks; }
    void setHasQuirks(bool value) { m_hasQuirks = value; }

    OpaqueByproducts& byproducts() { return *m_byproducts; }

    // Below are methods that make sense to call after you have generated code for the procedure.

    // You have to call this method after calling generate(). The code generated by B3::generate()
    // will require you to keep this object alive for as long as that code is runnable. Usually, this
    // just keeps alive things like the double constant pool and switch lookup tables. If this sounds
    // confusing, you should probably be using the JSC::Compilation API to compile code. If you use
    // that API, then you don't have to worry about this.
    std::unique_ptr<OpaqueByproducts> releaseByproducts() { return WTF::move(m_byproducts); }

    // This gives you direct access to Code. However, the idea is that clients of B3 shouldn't have to
    // call this. So, Procedure has some methods (below) that expose some Air::Code functionality.
    const Air::Code& code() const { return *m_code; }
    Air::Code& code() { return *m_code; }

    unsigned LEGACY_NODELETE callArgAreaSizeInBytes() const;
    void requestCallArgAreaSizeInBytes(unsigned size);

    // This tells the register allocators to stay away from this register.
    JS_EXPORT_PRIVATE void pinRegister(Reg);
    
    JS_EXPORT_PRIVATE void LEGACY_NODELETE setOptLevel(unsigned value);
    unsigned optLevel() const { return m_optLevel; }

    const Policy& policy() const { return m_policy; }
    // A load is not folded into an instruction that comes after a write to this range.
    const HeapRange& heapThatLoadsStayInFrontOf() const { return m_heapThatLoadsStayInFrontOf; }
    void setHeapThatLoadsStayInFrontOf(const HeapRange& range) { m_heapThatLoadsStayInFrontOf = range; }
    const HeapRange& heapOfStackSlots() const { return m_heapOfStackSlots; }
    void setHeapOfStackSlots(const HeapRange& range) { m_heapOfStackSlots = range; }
    uint64_t limit(Limit limit) const { return m_policy.limits.get(limit); }
    HeapSetTable& heapSets() LIFETIME_BOUND { return m_heapSets; }
    Arena* arenaOrNull() LIFETIME_BOUND { return m_policy.allocation.usesArena ? &m_arena : nullptr; }
    bool hasEverAdded(Opcode opcode) const { return m_opcodesEverAdded.get(opcode); }
    template<typename Predicate>
    bool hasEverAddedAny(const Predicate& predicate) const
    {
        for (size_t opcode : m_opcodesEverAdded) {
            if (predicate(static_cast<Opcode>(opcode)))
                return true;
        }
        return false;
    }
    void didAddOpcode(Opcode opcode) { m_opcodesEverAdded.set(opcode); }

    JS_EXPORT_PRIVATE void fail(FailureKind, Origin = Origin(), String detail = { });
    const CompilationReport& report() const { return m_report; }
    void didReachLimit(Limit limit, Origin origin = Origin()) const
    {
        LimitHit& hit = m_report.limitHits[static_cast<size_t>(limit)];
        if (!hit.count++)
            hit.firstOrigin = origin;
    }
    JS_EXPORT_PRIVATE void addRemark(const Remark&, uint32_t count = 1);
    void count(Counter counter, uint32_t amount = 1) { m_report.counters[static_cast<size_t>(counter)] += amount; }
    
    // You can turn off used registers calculation. This may speed up compilation a bit. But if
    // you turn it off then you cannot use StackmapGenerationParams::usedRegisters() or
    // StackmapGenerationParams::unavailableRegisters().
    void setNeedsUsedRegisters(bool value) { m_needsUsedRegisters = value; }
    bool needsUsedRegisters() const { return m_needsUsedRegisters; }

    // Some patchpoint says StackmapValue::releasesDeadValues().
    void setReleasesDeadValues(bool value) { m_releasesDeadValues = value; }
    bool releasesDeadValues() const { return m_releasesDeadValues; }

    JS_EXPORT_PRIVATE unsigned LEGACY_NODELETE frameSize() const;
    JS_EXPORT_PRIVATE RegisterAtOffsetList calleeSaveRegisterAtOffsetList() const;

    PCToOriginMap& pcToOriginMap() LIFETIME_BOUND { return m_pcToOriginMap; }
    PCToOriginMap releasePCToOriginMap()
    {
        RELEASE_ASSERT(needsPCToOriginMap());
        return WTF::move(m_pcToOriginMap);
    }

    JS_EXPORT_PRIVATE void setWasmBoundsCheckGenerator(RefPtr<WasmBoundsCheckGenerator>);

    template<typename Functor>
    void setWasmBoundsCheckGenerator(const Functor& functor)
    {
        setWasmBoundsCheckGenerator(RefPtr<WasmBoundsCheckGenerator>(createSharedTask<WasmBoundsCheckGeneratorFunction>(functor)));
    }

    JS_EXPORT_PRIVATE RegisterSet mutableGPRs();

    void LEGACY_NODELETE setNeedsPCToOriginMap();
    bool needsPCToOriginMap() { return m_needsPCToOriginMap; }

    JS_EXPORT_PRIVATE void freeUnneededB3ValuesAfterLowering();

    bool shouldDumpIR() const { return m_shouldDumpIR; }
    JS_EXPORT_PRIVATE void LEGACY_NODELETE setShouldDumpIR();

    void setUsesSIMD()
    { 
        RELEASE_ASSERT(m_policy.upstream.allowsSIMD);
        m_usesSIMD = true;
    }
    bool usesSIMD() const
    {
        // See also: WasmModuleInformation::usesSIMD().
        if (!m_policy.upstream.allowsSIMD)
            return false;
        if (m_policy.upstream.forcesSIMD)
            return true;
        return m_usesSIMD;
    }

    void setIonGraphPasses(Ref<JSON::Array>&&);
    void appendIonGraphPass(ASCIILiteral);

    AbstractHeapRepository& heaps()
    {
        if (!m_heaps) [[unlikely]]
            createHeaps();
        return *m_heaps;
    }
    const AbstractHeapRepository& heaps() const { return const_cast<Procedure*>(this)->heaps(); }

    void setIsWasm(bool flag) { m_isWasm = flag; }
    bool isWasm() const { return m_isWasm; }

    void setUsesWasmGCStructAllocations(bool flag = true) { m_usesWasmGCStructAllocations = flag; }
    bool usesWasmGCStructAllocations() const { return m_usesWasmGCStructAllocations; }
    void setUsesWasmGCArrayAllocations(bool flag = true) { m_usesWasmGCArrayAllocations = flag; }
    bool usesWasmGCArrayAllocations() const { return m_usesWasmGCArrayAllocations; }
    void setUsesWasmGCAccesses(bool flag = true) { m_usesWasmGCAccesses = flag; }
    bool usesWasmGCAccesses() const { return m_usesWasmGCAccesses; }

    void setUsesColdCCall(bool flag) { m_usesColdCCall = flag; }
    bool usesColdCCall() const { return m_usesColdCCall; }
    void setUsesShuffle(bool flag) { m_usesShuffle = flag; }
    bool usesShuffle() const { return m_usesShuffle; }
    void setUsesEntrySwitch(bool flag) { m_usesEntrySwitch = flag; }
    bool usesEntrySwitch() const { return m_usesEntrySwitch; }

private:
    friend class BlockInsertionSet;

    JS_EXPORT_PRIVATE Value* addValueImpl(Value*);
    void setBlockOrderImpl(Vector<BasicBlock*>&);
    JS_EXPORT_PRIVATE void createHeaps();

    template<typename T>
    void didAddPointerConstant(Origin origin, T* pointer)
    {
        if (pointer && m_policy.addressing.isPositionIndependent)
            fail(FailureKind::AbsoluteAddress, origin);
    }
    template<typename T>
    void didAddPointerConstant(Origin, T) { }

    Arena m_arena;
    SparseCollection<Variable> m_variables;
    Vector<Vector<Type>> m_tuples;
    Vector<std::unique_ptr<BasicBlock>> m_blocks;
    SparseCollection<Value> m_values;
    std::unique_ptr<CFG> m_cfg;
    std::unique_ptr<Dominators> m_dominators;
    std::unique_ptr<ValidatedForm> m_validatedForm;
    std::unique_ptr<NaturalLoops> m_naturalLoops;
    std::unique_ptr<BackwardsCFG> m_backwardsCFG;
    std::unique_ptr<BackwardsDominators> m_backwardsDominators;
    std::unique_ptr<BlockRarity> m_blockRarity;
    std::unique_ptr<FrequencyEstimate> m_frequencyEstimate;
    UncheckedKeyHashSet<ValueKey> m_fastConstants;
    const char* m_lastPhaseName;
    std::unique_ptr<OpaqueByproducts> m_byproducts;
    std::unique_ptr<Air::Code> m_code;
    std::unique_ptr<AbstractHeapRepository> m_heaps;
    RefPtr<SharedTask<void(PrintStream&, Origin)>> m_originPrinter;
    const void* m_frontendData;
    uint32_t m_subjectOfClient { 0 };
    String m_name;
    PCToOriginMap m_pcToOriginMap;
    RefPtr<JSON::Array> m_ionGraphPasses;
    unsigned m_numEntrypoints { 1 };
    unsigned m_optLevel { 2 };
    Policy m_policy;
    HeapRange m_heapThatLoadsStayInFrontOf;
    HeapRange m_heapOfStackSlots { HeapRange::top() };
    CompilationScratch* m_scratch { nullptr };
    mutable CompilationReport m_report;
    HeapSetTable m_heapSets;
    KnownProperties m_knownProperties;
    InductionRecords m_knownInductions;
    MovedLoadLog m_movedLoadLog;
    Vector<DataSectionRecord, 1> m_dataSections;
    Vector<std::unique_ptr<JumpTableRecord>> m_jumpTables;
    Vector<BasicBlock*> m_blocksInPreOrder;
    Vector<BasicBlock*> m_blocksInPostOrder;
    WTF::BitSet<256> m_opcodesEverAdded;
    bool m_hasCycle { false };
    bool m_areFrequenciesCurrent { false };
    bool m_needsUsedRegisters : 1 { true };
    bool m_releasesDeadValues : 1 { false };
    bool m_hasQuirks : 1 { false };
    bool m_needsPCToOriginMap : 1 { false };
    bool m_shouldDumpIR : 1 { false };
    bool m_usesSIMD : 1 { false };
    bool m_isWasm : 1 { false };
    bool m_usesWasmGCStructAllocations : 1 { false };
    bool m_usesWasmGCArrayAllocations : 1 { false };
    bool m_usesWasmGCAccesses : 1 { false };
    bool m_usesColdCCall : 1 { false };
    bool m_usesShuffle : 1 { false };
    bool m_usesEntrySwitch : 1 { false };
};
    
} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
