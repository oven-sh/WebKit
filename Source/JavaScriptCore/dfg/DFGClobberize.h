/*
 * Copyright (C) 2013-2021 Apple Inc. All rights reserved.
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

#if ENABLE(DFG_JIT)

#include "DFGAbstractHeap.h"
#include "DFGDoesGC.h"
#include "DFGGraph.h"
#include "DFGHeapLocation.h"
#include "DFGLazyNode.h"
#include "DFGPureValue.h"
#include "DOMJITCallDOMGetterSnippet.h"
#include "DOMJITSignature.h"
#include "InlineCallFrame.h"
#include "JSCellButterfly.h"

namespace JSC { namespace DFG {

// GIL-off, a node whose slow path can park on a heap-access release (a GC
// allocation blocking on the shared-GC handshake, the gated acquireHeapAccess
// resume, a transition-wait or compilation-lock spin) must clobber heap
// facts: the parked thread counts as quiescent, so a stop-the-world window
// (haveABadTime, structure retags, debugger JS) can complete over the park,
// and the rejoin carries no invalidation point or epoch check that would cut
// off a fact hoisted, CSE'd or propagated across the node. Flag-off and
// GIL-on this predicate is constant false: flag-off codegen is unchanged,
// and GIL-on the mutator holds the GIL across the slow path.
//
// Three consumers share this predicate and must never be gated differently:
// clobberize() emits a pre-switch write(Heap); AbstractInterpreter::
// executeEffects calls clobberStructures() so ConstantFoldingPhase keeps a
// CheckStructure/CheckArray that follows the park; clobbersExitState() skips
// exactly that injected write (it models cross-thread visibility, not an
// observable mutation by the node, and phases like TierUpCheckInjection
// insert predicate nodes with a preserved origin). The skip relies on the
// injected write being the FIRST write clobberize() reports.
//
// Phantom* allocation nodes are deliberately absent: they emit no code (the
// allocation was sunk; materialization happens at OSR exit), so there is no
// runtime park site under them.
inline bool jsThreadsParkableSlowPathClobbersHeapFacts(Graph& graph, Node* node)
{
    if (!Options::useJSThreads() || Options::useThreadGIL()) [[likely]]
        return false;
    switch (node->op()) {
    // GC-heap allocations modeled with write(HeapObjectCount) (the
    // allocation-class widening the audit's P10c row charters).
    case PushWithScope:
    case CreateActivation:
    case CreateDirectArguments:
    case CreateScopedArguments:
    case CreateClonedArguments:
    case CallObjectConstructor:
    case ToThis:
    case ArraySlice:
    case ArrayConcatArray:
    case ArrayConcatAppendOne:
    case AllocatePropertyStorage:
    case ReallocatePropertyStorage:
    case NewArrayWithSize:
    case NewArrayWithSizeAndStructure:
    case NewButterflyWithSize:
    case ArraySortCompact:
    case MaterializeNewArrayWithButterfly:
    case NewArrayWithButterfly:
    case NewArrayWithSpread:
    case NewArray:
    case NewArrayBuffer:
    case CreateRest:
    case ObjectCreate:
    case NewObject:
    case NewInternalFieldObject:
    case NewPromise:
    case NewRegExp:
    case NewStringObject:
    case NewMap:
    case NewSet:
    case NewWeakMap:
    case NewWeakSet:
    case MaterializeNewObject:
    case MaterializeNewInternalFieldObject:
    case MaterializeCreateActivation:
    case NewFunction:
    case NewGeneratorFunction:
    case NewAsyncGeneratorFunction:
    case NewAsyncFunction:
    case NewBoundFunction:
    // GC-cell allocators modeled pure (JSRopeString / JSString): the RESULT
    // stays PureValue-CSE-able (pure matches are not invalidated by heap
    // writes), but heap facts must not cross the allocation.
    case MakeRope:
    case MakeAtomString:
    case StrCat:
    // Butterfly (ArrayStorage) allocation on the conversion path.
    case Arrayify:
    case ArrayifyToStructure:
    // HashMap storage allocation / rehash.
    case SetAdd:
    case MapSet:
    case MapOrSetDelete:
    case WeakSetAdd:
    case WeakMapSet:
    // P10b: the tier-up service path can reach class-2 parks — Worklist plan
    // completion / installCode takes the gilOff compilation lock, whose wait
    // loop alternates parkSitePollAndParkForStopTheWorld with
    // handleTrapsForCurrentThreadIfNeeded (ScriptExecutable.cpp,
    // GILOffCompilationLocker). Those park primitives are epoch-bracketed
    // and jettison on overlap, but the rejoin back into this node carries no
    // invalidation point, so a hoisted fact consumed between the rejoin and
    // the next poll's IP (up to one loop iteration) is not provably cut off.
    // Cost containment: these nodes exist ONLY in DFG-tier plans, and LICM
    // runs only in FTL-mode plans (which never see CheckTierUp*), so the
    // loop-hoisting de-jank is unaffected; this only pessimizes DFG-local
    // CSE across tier-up checks.
    case CheckTierUpInLoop:
    case CheckTierUpAtReturn:
    case CheckTierUpAndOSREnter:
        return true;

    // Leg-dependent cases. Legs that already clobber top need no help; the
    // listed legs are the ones modeled precisely.
    case NewTypedArray:
    case NewTypedArrayBuffer:
        return node->child1().useKind() == Int32Use || node->child1().useKind() == Int52RepUse;
    case Spread:
        return node->child1()->op() == PhantomNewArrayBuffer || node->child1()->op() == PhantomCreateRest;
    case NewSymbol:
        return !node->child1() || node->child1().useKind() == StringUse;
    case NewRegExpUntyped:
        return node->child1().useKind() == StringUse && node->child2().useKind() == StringUse;
    case CompareEq:
    case CompareLess:
    case CompareLessEq:
    case CompareGreater:
    case CompareGreaterEq:
        // The string-compare leg can resolve ropes / allocate.
        return node->isBinaryUseKind(StringUse);
    case NewResolvedPromise:
        return node->isResolvedValueKnownNonThenable();
    case MultiPutByOffset:
        // Transitioning / storage-reallocating variants run a runtime slow
        // path that can allocate property storage AND park in the
        // JSObjectInlines transition-wait spin (P10b). Plain-replace
        // variants are inline stores with no parkable slow path.
        return node->multiPutByOffsetData().writesStructures() || node->multiPutByOffsetData().reallocatesStorage();

    // Parks by design, with no heap write: its GIL-off leg is an invalidation
    // point, and a park that overlaps a heap-fact rewrite jettisons the code
    // (see its case in clobberize()).
    case CheckTraps:
        return false;

    // Everything else that can allocate in the GC heap can park, because the
    // allocation can wait for a collection. The cases above name the nodes
    // whose reasons are not an allocation. Nodes that already clobber the
    // heap do not need this, and get it anyway.
    default:
        return doesGCIgnoringClobberize(graph, node);
    }
}

template<typename ReadFunctor, typename WriteFunctor, typename DefFunctor>
void clobberize(Graph& graph, Node* node, const ReadFunctor& read, const WriteFunctor& write, const DefFunctor& def)
{
    clobberize(graph, node, read, write, def, [] { });
}

template<typename ReadFunctor, typename WriteFunctor, typename DefFunctor, typename ClobberTopFunctor>
void clobberize(Graph& graph, Node* node, const ReadFunctor& read, const WriteFunctor& write, const DefFunctor& def, const ClobberTopFunctor& clobberTopFunctor)
{
    // Some notes:
    //
    // - The canonical way of clobbering the world is to read world and write
    //   heap. This is because World subsumes Heap and Stack, and Stack can be
    //   read by anyone but only written to by explicit stack writing operations.
    //   Of course, claiming to also write World is not wrong; it'll just
    //   pessimise some important optimizations.
    //
    // - We cannot hoist, or sink, anything that has effects. This means that the
    //   easiest way of indicating that something cannot be hoisted is to claim
    //   that it side-effects some miscellaneous thing.
    //
    // - Some nodes lie, and claim that they do not read the JSCell_structureID,
    //   JSCell_typeInfoFlags, etc. These are nodes that use the structure in a way
    //   that does not depend on things that change under structure transitions.
    //
    // - It's implicitly understood that OSR exits read the world. This is why we
    //   generally don't move or eliminate stores. Every node can exit, so the
    //   read set does not reflect things that would be read if we exited.
    //   Instead, the read set reflects what the node will have to read if it
    //   *doesn't* exit.
    //
    // - Broadly, we don't say that we're reading something if that something is
    //   immutable.
    //
    // - This must be sound even prior to type inference. We use this as early as
    //   bytecode parsing to determine at which points in the program it's legal to
    //   OSR exit.
    //
    // - If you do read(Stack) or read(World), then make sure that readTop() in
    //   PreciseLocalClobberize is correct.
    
    // While read() and write() are fairly self-explanatory - they track what sorts of things the
    // node may read or write - the def() functor is more tricky. It tells you the heap locations
    // (not just abstract heaps) that are defined by a node. A heap location comprises an abstract
    // heap, some nodes, and a LocationKind. Briefly, a location defined by a node is a location
    // whose value can be deduced from looking at the node itself. The locations returned must obey
    // the following properties:
    //
    // - If someone wants to CSE a load from the heap, then a HeapLocation object should be
    //   sufficient to find a single matching node.
    //
    // - The abstract heap is the only abstract heap that could be clobbered to invalidate any such
    //   CSE attempt. I.e. if clobberize() reports that on every path between some node and a node
    //   that defines a HeapLocation that it wanted, there were no writes to any abstract heap that
    //   overlap the location's heap, then we have a sound match. Effectively, the semantics of
    //   write() and def() are intertwined such that for them to be sound they must agree on what
    //   is CSEable.
    //
    // read(), write(), and def() for heap locations is enough to do GCSE on effectful things. To
    // keep things simple, this code will also def() pure things. def() must be overloaded to also
    // accept PureValue. This way, a client of clobberize() can implement GCSE entirely using the
    // information that clobberize() passes to write() and def(). Other clients of clobberize() can
    // just ignore def() by using a NoOpClobberize functor.

    // We allow the runtime to perform a stack scan at any time. We don't model which nodes get implemented
    // by calls into the runtime. For debugging we might replace the implementation of any node with a call
    // to the runtime, and that call may walk stack. Therefore, each node must read() anything that a stack
    // scan would read. That's what this does.
    for (InlineCallFrame* inlineCallFrame = node->origin.semantic.inlineCallFrame(); inlineCallFrame; inlineCallFrame = inlineCallFrame->directCaller.inlineCallFrame()) {
        if (inlineCallFrame->isClosureCall)
            read(AbstractHeap(Stack, VirtualRegister(inlineCallFrame->stackOffset + CallFrameSlot::callee)));
        if (inlineCallFrame->isVarargs())
            read(AbstractHeap(Stack, VirtualRegister(inlineCallFrame->stackOffset + CallFrameSlot::argumentCountIncludingThis)));
    }

    // We don't want to specifically account which nodes can read from the scope
    // when the debugger is enabled. It's helpful to just claim all nodes do.
    // Specifically, if a node allocates, this may call into the debugger's machinery.
    // The debugger's machinery is free to take a stack trace and try to read from
    // a scope which is expected to be flushed to the stack.
    if (graph.hasDebuggerEnabled()) {
        ASSERT(!node->origin.semantic.inlineCallFrame());
        read(AbstractHeap(Stack, graph.m_codeBlock->scopeRegister()));
    }

    auto clobberTop = [&] {
        if (Options::validateDFGClobberize())
            clobberTopFunctor();
        read(World);
        write(Heap);
    };

    // GIL-off, parkable slow paths clobber heap facts (see the predicate's
    // comment above). This pre-switch write is processed before any def()
    // the node's own case emits, so the node's own results (e.g. NewArray's
    // ArrayLengthLoc def) stay CSE-able while every prior heap-fact
    // availability is killed. The abstract interpreter consumes the same
    // predicate with clobberStructures().
    if (jsThreadsParkableSlowPathClobbersHeapFacts(graph, node)) [[unlikely]]
        write(Heap);

    // Since Fixup can widen our ArrayModes based on profiling from other nodes we pessimistically assume
    // all nodes with an ArrayMode can clobber top. We allow some nodes like CheckArray because they can
    // only exit.
    if (graph.m_planStage < PlanStage::AfterFixup && node->hasArrayMode()) {
        switch (node->op()) {
        case CheckArray:
        case CheckArrayOrEmpty:
            break;
        case EnumeratorNextUpdateIndexAndMode:
        case EnumeratorGetByVal:
        case EnumeratorPutByVal:
        case EnumeratorInByVal:
        case EnumeratorHasOwnProperty:
        case GetIndexedPropertyStorage:
        case DataViewGetByteLength:
        case DataViewGetByteLengthAsInt52:
        case GetArrayLength:
        case GetUndetachedTypeArrayLength:
        case GetTypedArrayLengthAsInt52:
        case GetTypedArrayByteOffset:
        case GetTypedArrayByteOffsetAsInt52:
        case GetVectorLength:
        case InByVal:
        case InByValMegamorphic:
        case PutByValDirect:
        case PutByVal:
        case PutByValDirectResolved:
        case PutByValMegamorphic:
        case GetByVal:
        case GetByValMegamorphic:
        case MultiGetByVal:
        case MultiPutByVal:
        case StringAt:
        case StringCharAt:
        case StringCharCodeAt:
        case StringCodePointAt:
        case Arrayify:
        case ArrayifyToStructure:
        case ArrayPush:
        case ArrayPop:
        case ArrayShift:
        case ArrayUnshift:
        case ArrayIncludes:
        case ArrayIndexOf:
        case ArrayJoin:
        case HasIndexedProperty:
        case AtomicsAdd:
        case AtomicsAnd:
        case AtomicsCompareExchange:
        case AtomicsExchange:
        case AtomicsLoad:
        case AtomicsOr:
        case AtomicsStore:
        case AtomicsSub:
        case AtomicsXor:
        case NewArrayWithSpecies:
        case ArraySortCompact:
        case ArraySortCommit:
        case GetCellButterflySlot:
        case BufferReadInt:
        case BufferReadFloat:
        case BufferWrite:
            return clobberTop();
        default:
            DFG_CRASH(graph, node, "Unhandled ArrayMode opcode.");
        }
    }
    
    switch (node->op()) {
    case JSConstant:
    case DoubleConstant:
    case Int52Constant:
        def(PureValue(node, node->constant()));
        return;

    case Identity:
    case IdentityWithProfile:
    case Phantom:
    case Check:
    case CheckVarargs:
    case ExtractOSREntryLocal:
    case CheckStructureImmediate:
        return;

    case ExtractCatchLocal:
        read(AbstractHeap(CatchLocals, node->catchOSREntryIndex()));
        return;

    case ClearCatchLocals:
        write(CatchLocals);
        return;
        
    case LazyJSConstant:
        // We should enable CSE of LazyJSConstant. It's a little annoying since LazyJSValue has
        // more bits than we currently have in PureValue.
        return;

    case CompareEqPtr:
        def(PureValue(node, node->cellOperand()->cell()));
        return;

    case UnwrapGlobalProxy:
        read(JSGlobalProxy_target);
        def(HeapLocation(GlobalProxyTargetLoc, JSGlobalProxy_target, node->child1()), LazyNode(node));
        return;

    case ArithIMul:
    case ArithPow:
    case GetScope:
    case SkipScope:
    case GetGlobalObject:
    case StringCharCodeAt:
    case StringCodePointAt:
    case StringIndexOf:
    case StringLastIndexOf:
    case StringStartsWith:
    case StringEndsWith:
    case CompareStrictEq:
    case SameValue:
    case IsEmpty:
    case IsEmptyStorage:
    case IsUndefinedOrNull:
    case IsBoolean:
    case IsNumber:
    case IsBigInt:
    case NumberIsInteger:
    case IsObject:
    case CheckInBounds:
    case CheckInBoundsInt52:
    case DoubleRep:
    case PurifyNaN:
    case ValueRep:
    case Int52Rep:
    case BooleanToNumber:
    case FiatInt52:
    case MakeRope:
    case MakeAtomString:
    case StrCat:
    case ValueToInt32:
    case GetExecutable:
    case BottomValue:
    case SymbolToString:
        def(PureValue(node));
        return;

    // These nodes are realm-dependent when MasqueradesAsUndefined is involved.
    // Including the globalObject in the PureValue ensures nodes from different realms are not folded by CSE.
    case TypeOfIsUndefined:
    case ToBoolean:
    case LogicalNot:
    case TypeOf:
        def(PureValue(node, graph.globalObjectFor(node->origin.semantic)));
        return;

    // JSCallee for Eval can change the scope field.
    case GetEvalScope:
        read(World);
        return;

    case NumberIsFinite:
    case NumberIsNaN:
    case NumberIsSafeInteger:
        def(PureValue(node));
        return;

    case GlobalIsFinite:
    case GlobalIsNaN:
        ASSERT(node->child1().useKind() == UntypedUse);
        clobberTop();
        return;

    case StringLocaleCompare:
        read(World);
        write(SideState);
        def(PureValue(node));
        return;

    case ArithMin:
    case ArithMax:
        def(PureValue(graph, node));
        return;

    case GetGlobalThis:
        read(World);
        return;

    case AtomicsIsLockFree:
        if (graph.child(node, 0).useKind() == Int32Use)
            def(PureValue(graph, node));
        else
            clobberTop();
        return;
        
    case ArithUnary:
        if (node->child1().useKind() == DoubleRepUse)
            def(PureValue(node, static_cast<std::underlying_type<Arith::UnaryType>::type>(node->arithUnaryType())));
        else
            clobberTop();
        return;

    case ArithFRound:
    case ArithF16Round:
    case ArithSqrt:
        if (node->child1().useKind() == DoubleRepUse)
            def(PureValue(node));
        else
            clobberTop();
        return;

    case ArithAbs:
        if (node->child1().useKind() == Int32Use || node->child1().useKind() == DoubleRepUse)
            def(PureValue(node, node->arithMode()));
        else
            clobberTop();
        return;

    case ArithClz32:
        if (node->child1().useKind() == Int32Use || node->child1().useKind() == KnownInt32Use)
            def(PureValue(node));
        else
            clobberTop();
        return;

    case ArithNegate:
        if (node->child1().useKind() == Int32Use
            || node->child1().useKind() == DoubleRepUse
            || node->child1().useKind() == Int52RepUse)
            def(PureValue(node, node->arithMode()));
        else
            clobberTop();
        return;

    case IsCellWithType:
        def(PureValue(node, node->queriedType().rawValue()));
        return;

    case ValueBitNot:
        if (node->child1().useKind() == AnyBigIntUse || node->child1().useKind() == BigInt32Use || node->child1().useKind() == HeapBigIntUse) {
            def(PureValue(node));
            return;
        }
        clobberTop();
        return;

    case ArithBitNot:
        if (node->child1().useKind() == UntypedUse) {
            clobberTop();
            return;
        }
        def(PureValue(node));
        return;

    case ArithBitAnd:
    case ArithBitOr:
    case ArithBitXor:
    case ArithBitLShift:
    case ArithBitRShift:
    case ArithBitURShift:
        if (node->child1().useKind() == UntypedUse || node->child2().useKind() == UntypedUse) {
            clobberTop();
            return;
        }
        def(PureValue(node));
        return;

    case ArithRandom:
        read(MathDotRandomState);
        write(MathDotRandomState);
        return;

    case DateNow:
        read(WallClock);
        write(WallClock);
        return;

    case EnumeratorNextUpdatePropertyName: {
        def(PureValue(node, node->enumeratorMetadata().toRaw()));
        return;
    }

    case ExtractFromTuple: {
        def(PureValue(node, node->extractOffset()));
        return;
    }

    case EnumeratorNextUpdateIndexAndMode:
    case HasIndexedProperty: {
        if (node->op() == EnumeratorNextUpdateIndexAndMode) {
            if (node->enumeratorMetadata() == JSPropertyNameEnumerator::OwnStructureMode && graph.varArgChild(node, 0).useKind() == CellUse) {
                read(JSObject_butterfly);
                read(NamedProperties);
                read(JSCell_structureID);
                return;
            }

            if (node->enumeratorMetadata() != JSPropertyNameEnumerator::IndexedMode) {
                clobberTop();
                return;
            }
        }

        read(JSObject_butterfly);
        ArrayMode mode = node->arrayMode();
        LocationKind locationKind = node->op() == EnumeratorNextUpdateIndexAndMode ? EnumeratorNextUpdateIndexAndModeLoc : HasIndexedPropertyLoc;
        switch (mode.type()) {
        case Array::ForceExit: {
            write(SideState);
            return;
        }
        case Array::Int32: {
            if (mode.isInBounds()) {
                read(Butterfly_publicLength);
                read(IndexedInt32Properties);
                def(HeapLocation(locationKind, IndexedInt32Properties, graph.varArgChild(node, 0), graph.varArgChild(node, 1)), LazyNode(node));
                return;
            }
            break;
        }
            
        case Array::Double: {
            if (mode.isInBounds()) {
                read(Butterfly_publicLength);
                read(IndexedDoubleProperties);
                def(HeapLocation(locationKind, IndexedDoubleProperties, graph.varArgChild(node, 0), graph.varArgChild(node, 1)), LazyNode(node));
                return;
            }
            break;
        }
            
        case Array::Contiguous: {
            if (mode.isInBounds()) {
                read(Butterfly_publicLength);
                read(IndexedContiguousProperties);
                def(HeapLocation(locationKind, IndexedContiguousProperties, graph.varArgChild(node, 0), graph.varArgChild(node, 1)), LazyNode(node));
                return;
            }
            break;
        }

        case Array::ArrayStorage: {
            if (mode.isInBounds()) {
                read(Butterfly_vectorLength);
                read(IndexedArrayStorageProperties);
                return;
            }
            break;
        }

        default:
            break;
        }

        clobberTop();
        return;
    }

    case StringFromCharCode:
        switch (node->child1().useKind()) {
        case Int32Use:
        case KnownInt32Use:
            def(PureValue(node));
            return;
        case UntypedUse:
            clobberTop();
            return;
        default:
            DFG_CRASH(graph, node, "Bad use kind");
        }
        return;

    case StringFromCodePoint:
        switch (node->child1().useKind()) {
        case Int32Use:
        case KnownInt32Use:
            // Can throw a RangeError for an out-of-range code point, so this is not pure.
            read(World);
            write(SideState);
            def(PureValue(node));
            return;
        case UntypedUse:
            clobberTop();
            return;
        default:
            DFG_CRASH(graph, node, "Bad use kind");
        }
        return;

    case ArithAdd:
    case DoubleAsInt32:
    case UInt32ToNumber:
        def(PureValue(node, node->arithMode()));
        return;

    case ArithDiv:
    case ArithMod:
    case ArithMul:
    case ArithSub:
        switch (node->binaryUseKind()) {
        case Int32Use:
        case Int52RepUse:
        case DoubleRepUse:
            def(PureValue(node, node->arithMode()));
            return;
        case UntypedUse:
            clobberTop();
            return;
        default:
            DFG_CRASH(graph, node, "Bad use kind");
        }

    case ArithRound:
    case ArithFloor:
    case ArithCeil:
    case ArithTrunc:
        if (node->child1().useKind() == DoubleRepUse)
            def(PureValue(node, static_cast<uintptr_t>(node->arithRoundingMode())));
        else
            clobberTop();
        return;

    case CheckIsConstant:
        def(PureValue(CheckIsConstant, AdjacencyList(AdjacencyList::Fixed, node->child1()), node->constant()));
        return;

    case CheckNotEmpty:
        def(PureValue(CheckNotEmpty, AdjacencyList(AdjacencyList::Fixed, node->child1())));
        return;

    case AssertInBounds:
    case AssertNotEmpty:
        write(SideState);
        return;

    case CheckIdent:
        def(PureValue(CheckIdent, AdjacencyList(AdjacencyList::Fixed, node->child1()), node->uidOperand()));
        return;

    case ConstantStoragePointer:
        def(PureValue(node, node->storagePointer()));
        return;

    case KillStack:
        write(AbstractHeap(Stack, node->unlinkedOperand()));
        return;
         
    case MovHint:
    case ZombieHint:
    case ExitOK:
    case Upsilon:
    case Phi:
    case PhantomLocal:
    case SetArgumentDefinitely:
    case SetArgumentMaybe:
    case Jump:
    case Branch:
    case Switch:
    case EntrySwitch:
    case ForceOSRExit:
    case CPUIntrinsic:
    case CheckBadValue:
    case Return:
    case Unreachable:
    case LoopHint:
    case ProfileType:
    case ProfileControlFlow:
    case PutHint:
    case InitializeEntrypointArguments:
    case FilterCallLinkStatus:
    case FilterGetByStatus:
    case FilterPutByStatus:
    case FilterInByStatus:
    case FilterDeleteByStatus:
    case FilterCheckPrivateBrandStatus:
    case FilterSetPrivateBrandStatus:
        write(SideState);
        return;
        
    case CheckTierUpInLoop:
    case CheckTierUpAtReturn:
    case CheckTierUpAndOSREnter:
        // AUDIT-checktraps P10b (amend round 2): the tier-up service path can
        // reach class-2 parks — Worklist plan completion / installCode takes
        // the gilOff compilation lock, whose wait loop alternates
        // parkSitePollAndParkForStopTheWorld with
        // handleTrapsForCurrentThreadIfNeeded (ScriptExecutable.cpp,
        // GILOffCompilationLocker). Those park primitives are epoch-bracketed
        // and jettison on overlap, but the rejoin back into this node carries
        // no invalidation point, so a hoisted fact consumed between the
        // rejoin and the next poll's IP (up to one loop iteration) is not
        // provably cut off. The GIL-off heap-fact clobber comes from the
        // pre-switch jsThreadsParkableSlowPathClobbersHeapFacts() write; see
        // the predicate's CheckTierUp* comment for the cost-containment
        // argument (DFG-only nodes; FTL/LICM never sees them). Flag-off/
        // GIL-on: predicate is false, unchanged model.
        write(SideState);
        return;

    case StoreBarrier:
        read(JSCell_cellState);
        write(JSCell_cellState);
        return;
        
    case FencedStoreBarrier:
        read(Heap);
        write(JSCell_cellState);
        return;

    case CheckTraps:
        read(InternalState);
        write(InternalState);
        if (Options::useJSThreads()) [[unlikely]] {
            // UNGIL §K.5 / SPEC-jit I21 (AB-10 closure): flag-on, the polling
            // CheckTraps is a PARK SITE — a mutator that traps here parks for
            // the whole §A.3 thread-granular window (or a Mode-machine stop),
            // during which the conductor may rewrite ANY heap fact this code
            // hoisted: haveABadTime converts every fast-indexing butterfly to
            // (SlowPut)ArrayStorage, Class-A fires retag structures, debugger
            // services run arbitrary JS.
            //
            // checktraps-dejank-invalidation-point: GIL-off, the park-site
            // hazard is enforced by PRECISE JETTISON + an invalidation point
            // instead of a clobberWorld haymaker. CheckTraps codegen emits an
            // InvalidationPoint-shaped jump-replacement watchpoint at the
            // poll's rejoin (DFG SpeculativeJIT::compileCheckTraps / FTL
            // compileCheckTraps), and every conductor-side heap-fact rewrite
            // that can occur inside a stop window bumps the conductor
            // heap-fact rewrite epoch (JSThreadsSafepoint::
            // noteConductorHeapFactRewrite) IN-WINDOW, BEFORE the stopped
            // world resumes — the wrapped-work closure in
            // stopTheWorldAndRun's gilOff reroute, except pure
            // code-lifecycle jettisons, plus context-edge bumps and explicit
            // bumps at haveABadTimeImpl and the debugger-break service (the
            // bump-edge law and full site table: BUMP-EDGE LAW comment in
            // bytecode/JSThreadsSafepoint.cpp and
            // docs/threads/AUDIT-checktraps.md). A mutator
            // whose park in VMTraps::handleTraps overlapped a bump jettisons
            // its own on-stack optimizing-JIT code on resume, which fires
            // this node's invalidation point, so execution OSR-exits at the
            // poll BEFORE any hoisted heap fact (CheckArray/GetButterfly/
            // structure) is reused against the rewritten heap. Hence heap
            // facts legally survive the poll at compile time: model the node
            // exactly like InvalidationPoint below, plus the poll's own
            // InternalState traffic.
            //
            // ORDER IS LOAD-BEARING: write(Watchpoint_fire) precedes the
            // def(). The write kills any InvalidationPointLoc availability
            // established by an earlier InvalidationPoint/CheckTraps
            // (including this node's own def reaching around a loop
            // backedge), so CSE can never replace THIS CheckTraps with an
            // earlier invalidation point and silently DELETE the poll — a
            // dropped poll is a missed safepoint (STW watchdog timeout /
            // GIL never yielded). A later plain InvalidationPoint may be
            // CSE'd into this node (the def): that is sound because codegen
            // emits a real watchpoint label here whenever this def is
            // emitted (gate must match: useJSThreads && !useThreadGIL &&
            // node->origin.exitOK at all three sites + AI).
            //
            // GIL-on flag-on keeps the conservative model: the GIL park/
            // hand-off edges are not all routed through handleTraps' epoch
            // check, so invalidation coverage is not proven there; clobbering
            // is trivially correct. Flag-off this whole branch is dead
            // (byte-identical codegen LAW).
            if (!Options::useThreadGIL() && node->origin.exitOK) {
                write(Watchpoint_fire);
                write(SideState);
                // AUDIT-checktraps §7.1 INTERIM DEFAULT (amend round 2),
                // pending the threads memory-model ruling on poll-bounded
                // visibility of plain writes: keep USER-VISIBLE MUTABLE DATA
                // non-hoistable across the poll. Without these writes, LICM
                // may legally hoist a loop-invariant plain-field/element/
                // global/closure-var load out of a hot GIL-off loop, so a
                // spin-on-plain-flag loop (no Atomics) hangs — a user-visible
                // semantic regression the old read(World)/write(Heap) model
                // de-facto prevented. This is the "write(NamedProperties)-
                // grade clobber / partial de-jank" outcome §7.1 anticipates
                // for a YES ruling; if the ruling lands NO (plain reads may
                // be hoisted), delete this block and adjust the corpus. The
                // de-jank this change targets is preserved for the SHAPE
                // facts that ARE precise-jettison-covered: structure,
                // indexing type, typeinfo remain hoistable across polls,
                // guarded by the invalidation point + precise jettison. The
                // butterfly POINTER and vectorLength are NOT hoistable —
                // see the JSObject_butterfly write below (Tier-B B3).
                // (IndexedProperties is the supertype of all element-like
                // heaps: the four indexed kinds, DirectArguments, Scope, and
                // TypedArray properties — plain typed-array element reads
                // stay poll-bounded too, because in this design plain
                // (non-SAB) ArrayBuffers are shareable across Threads and
                // get no coverage from the ECMA SAB memory model.)
                write(NamedProperties);
                write(IndexedProperties);
                write(Butterfly_publicLength);
                // Tier-B B3 / map-MC-JIT.md S2(a) (B3-JIT-POLL-CLOBBER-LINT):
                // the BUTTERFLY POINTER itself MUST NOT survive the poll in
                // unregistered code. The precise-jettison story above covers
                // structure/indexingType — every falsifier of those goes
                // through a stop window and bumps the conductor heap-fact
                // rewrite epoch (arms (b)/(c) of map-MC-JIT.md S2). It does
                // NOT cover arm (a): a foreign flat->segmented conversion +
                // growth on an object whose TTL sets are already fired is a
                // lock-free, NO-STW operation (OM 4.2) — no epoch bump, no
                // jettison — yet it re-tags the butterfly word and (via the
                // aliased fragment-0-slot-0 = flat IndexingHeader) grows the
                // live publicLength past the frozen flat-era vectorLength
                // (OM I9b). A LICM-hoisted (stale) masked flat base + a
                // re-loaded (fresh) publicLength is then a heap OOB
                // (CVE-2019-5782 / CVE-2021-2388 analog;
                // JSTests/threads/cve/mc-jit-stale-base-grow-oob.js). Writing
                // JSObject_butterfly + Butterfly_vectorLength here forces
                // SAME-SNAPSHOT {base, publicLength, vectorLength} per poll
                // boundary — the sufficient condition for S3's
                // immune-by-construction BCE argument — so any staleness is
                // memory-safe (old allocation kept live by conservative scan,
                // never shrunk in place; jit R2 / OM I7). Structure /
                // indexingType remain hoistable: the de-jank's chief win
                // (CheckArray/CheckStructure surviving the poll) is intact.
                // The validateButterflyTagDisciplineForGraph lint (declared
                // below; SPEC-jit I14/I21(b) Task-13) cross-checks that no
                // GetButterfly result is consumed across this clobber.
                write(JSObject_butterfly);
                write(Butterfly_vectorLength);
                // Typed-array view fields (vector, length, byteOffset) and the
                // other MiscFields-defined facts must be re-loaded after every
                // poll: a foreign detach or transfer zeroes the view under only
                // its cell lock, and the old mapping it quarantines is released
                // at the next stop, which this poll may be parked in. A hoisted
                // {vector, length} pair would otherwise survive the poll and
                // index freed memory.
                write(MiscFields);
                write(Absolute);
                write(JSMapFields);
                write(JSSetFields);
                write(JSWeakMapFields);
                write(JSWeakSetFields);
                write(JSInternalFields);
                write(JSDateFields);
                write(RegExpObject_lastIndex);
                def(HeapLocation(InvalidationPointLoc, Watchpoint_fire), LazyNode(node));
                return;
            }
            read(World);
            write(Heap);
        }
        return;

    case InvalidationPoint:
        write(SideState);
        // A trap-check InvalidationPoint must stay where it is (see Node::isVMTrapsBreakpointSite()); every other
        // one is redundant with a dominating InvalidationPoint that no watchpoint fire separates it from.
        if (!node->isVMTrapsBreakpointSite())
            def(HeapLocation(InvalidationPointLoc, Watchpoint_fire), LazyNode(node));
        return;

    case Flush:
        read(AbstractHeap(Stack, node->operand()));
        write(SideState);
        return;

    case NotifyWrite:
        write(Watchpoint_fire);
        write(SideState);
        return;

    case PushWithScope: {
        read(World);
        write(HeapObjectCount);
        return;
    }

    case CreateActivation: {
        SymbolTable* table = node->castOperand<SymbolTable*>();
        if (table->singleton().isStillValid())
            write(Watchpoint_fire);
        read(HeapObjectCount);
        write(HeapObjectCount);
        return;
    }

    case CreateDirectArguments:
    case CreateScopedArguments:
    case CreateClonedArguments:
        read(Stack);
        read(HeapObjectCount);
        write(HeapObjectCount);
        return;

    case PhantomDirectArguments:
    case PhantomClonedArguments:
        // DFG backend requires that the locals that this reads are flushed. FTL backend can handle those
        // locals being promoted.
        if (!graph.m_plan.isFTL())
            read(Stack);
        
        // Even though it's phantom, it still has the property that one can't be replaced with another.
        read(HeapObjectCount);
        write(HeapObjectCount);
        return;

    case PhantomSpread:
    case PhantomNewArrayWithSpread:
    case PhantomNewArrayBuffer:
    case PhantomCreateRest:
        // Even though it's phantom, it still has the property that one can't be replaced with another.
        read(HeapObjectCount);
        write(HeapObjectCount);
        return;

    case CallObjectConstructor:
        read(HeapObjectCount);
        write(HeapObjectCount);
        return;

    case ToThis:
        read(MiscFields);
        read(HeapObjectCount);
        write(HeapObjectCount);
        return;

    case TypeOfIsObject:
        read(MiscFields);
        def(HeapLocation(TypeOfIsObjectLoc, MiscFields, node->child1(), graph.globalObjectFor(node->origin.semantic)), LazyNode(node));
        return;

    case TypeOfIsFunction:
        read(MiscFields);
        def(HeapLocation(TypeOfIsFunctionLoc, MiscFields, node->child1(), graph.globalObjectFor(node->origin.semantic)), LazyNode(node));
        return;
        
    case IsCallable:
        read(MiscFields);
        def(HeapLocation(IsCallableLoc, MiscFields, node->child1()), LazyNode(node));
        return;

    case IsConstructor:
        read(MiscFields);
        def(HeapLocation(IsConstructorLoc, MiscFields, node->child1()), LazyNode(node));
        return;

    case ArrayIsArray:
        read(MiscFields);
        write(SideState);
        def(HeapLocation(ArrayIsArrayLoc, MiscFields, node->child1()), LazyNode(node));
        return;

    case MatchStructure:
        read(JSCell_structureID);
        return;

    case ArraySlice:
        read(MiscFields);
        read(JSCell_indexingType);
        read(JSCell_structureID);
        read(JSObject_butterfly);
        read(Butterfly_publicLength);
        read(IndexedDoubleProperties);
        read(IndexedInt32Properties);
        read(IndexedContiguousProperties);
        read(HeapObjectCount);
        write(HeapObjectCount);
        return;

    case ArrayConcatArray:
    case ArrayConcatAppendOne: {
        read(MiscFields);
        read(JSCell_indexingType);
        read(JSCell_structureID);
        read(JSObject_butterfly);
        read(Butterfly_publicLength);
        read(IndexedDoubleProperties);
        read(IndexedInt32Properties);
        read(IndexedContiguousProperties);
        read(IndexedArrayStorageProperties);
        read(HeapObjectCount);
        write(HeapObjectCount);
        return;
    }

    case ArrayIncludes:
    case ArrayIndexOf: {
        // FIXME: Should support a CSE rule.
        // https://bugs.webkit.org/show_bug.cgi?id=173173
        read(MiscFields);
        read(JSCell_indexingType);
        read(JSCell_structureID);
        read(JSObject_butterfly);
        read(Butterfly_publicLength);
        switch (node->arrayMode().type()) {
        case Array::Double:
            read(IndexedDoubleProperties);
            return;
        case Array::Int32:
            read(IndexedInt32Properties);
            return;
        case Array::Contiguous:
            read(IndexedContiguousProperties);
            return;
        default:
            RELEASE_ASSERT_NOT_REACHED();
            return;
        }
        return;
    }

    case ArrayJoin: {
        clobberTop();
        return;
    }

    case GetById:
    case GetByIdFlush:
    case GetByIdMegamorphic:
    case GetByIdWithThis:
    case GetByIdWithThisMegamorphic:
    case GetByIdDirect:
    case GetByIdDirectFlush:
    case GetByValWithThis:
    case GetByValWithThisMegamorphic:
    case PutById:
    case PutByIdMegamorphic:
    case PutByIdWithThis:
    case PutByValWithThis:
    case PutByIdFlush:
    case PutByIdDirect:
    case PutGetterById:
    case PutSetterById:
    case PutGetterSetterById:
    case PutGetterByVal:
    case PutSetterByVal:
    case PutPrivateName:
    case PutPrivateNameById:
    case GetPrivateName:
    case GetPrivateNameById:
    case DefineDataProperty:
    case DefineAccessorProperty:
    case ObjectDefineProperty:
    case ObjectDefinePropertyFromFields:
    case DeleteById:
    case DeleteByVal:
    case ArrayPush:
    case ArrayPop:
    case ArrayShift:
    case ArrayUnshift:
    case ArraySplice:
    case Call:
    case DirectCall:
    case TailCallInlinedCaller:
    case DirectTailCallInlinedCaller:
    case Construct:
    case DirectConstruct:
    case CallVarargs:
    case CallForwardVarargs:
    case TailCallVarargsInlinedCaller:
    case TailCallForwardVarargsInlinedCaller:
    case ConstructVarargs:
    case ConstructForwardVarargs:
    case CallDirectEval:
    case CallWasm:
    case TailCallInlinedCallerWasm:
    case CallFFI:
    case CallCustomAccessorGetter:
    case CallCustomAccessorSetter:
    case ToPrimitive:
    case ToPropertyKey:
    case ToPropertyKeyOrNumber:
    case InByVal:
    case InByValMegamorphic:
    case EnumeratorInByVal:
    case EnumeratorHasOwnProperty:
    case InById:
    case InByIdMegamorphic:
    case HasPrivateName:
    case HasPrivateBrand:
    case HasOwnProperty:
    case ValueNegate:
    case SetFunctionName:
    case EnqueueAsyncGeneratorDriver:
    case GetDynamicVar:
    case PutDynamicVar:
    case ResolveScopeForHoistingFuncDeclInEval:
    case ResolveScope:
    case ToObject:
    case OpenAsyncFromSyncIterator:
    case GetPropertyEnumerator:
    case InstanceOfCustom:
    case ToNumeric:
    case NumberToStringWithRadix:
    case CreateThis:
    case CreatePromise:
    case CreateGenerator:
    case CreateAsyncGenerator:
    case InstanceOf:
    case InstanceOfMegamorphic:
    case ObjectKeys:
    case ObjectGetOwnPropertyNames:
    case ObjectGetOwnPropertySymbols:
    case ObjectToString:
    case ReflectOwnKeys:
        clobberTop();
        return;

    case StringValueOf:
        switch (node->child1().useKind()) {
        case StringOrOtherUse:
            read(World);
            write(SideState);
            def(PureValue(node));
            return;
        default:
            clobberTop();
            return;
        }

    case ToNumber:
        switch (node->child1().useKind()) {
        case StringUse:
            def(PureValue(node));
            return;
        default:
            clobberTop();
            return;
        }

    case CallNumberConstructor:
        switch (node->child1().useKind()) {
        case BigInt32Use:
            def(PureValue(node));
            return;
        case UntypedUse:
            clobberTop();
            return;
        default:
            DFG_CRASH(graph, node, "Bad use kind");
        }

    case Inc:
    case Dec:
        switch (node->child1().useKind()) {
        case Int32Use:
        case Int52RepUse:
        case DoubleRepUse:
        case BigInt32Use:
        case HeapBigIntUse:
        case AnyBigIntUse:
            def(PureValue(node));
            return;
        case UntypedUse:
            clobberTop();
            return;
        default:
            DFG_CRASH(graph, node, "Bad use kind");
        }

    case ValueBitAnd:
    case ValueBitXor:
    case ValueBitOr:
    case ValueAdd:
    case ValueSub:
    case ValueMul:
    case ValueDiv:
    case ValueMod:
    case ValuePow:
    case ValueBitLShift:
    case ValueBitRShift:
        // FIXME: this use of single-argument isBinaryUseKind would prevent us from specializing (for example) for a HeapBigInt left-operand and a BigInt32 right-operand.
        if (node->isBinaryUseKind(AnyBigIntUse) || node->isBinaryUseKind(BigInt32Use) || node->isBinaryUseKind(HeapBigIntUse)) {
            read(World);
            write(SideState);
            def(PureValue(node));
            return;
        }
        clobberTop();
        return;

    case ValueBitURShift:
        // URShift >>> does not accept BigInt.
        clobberTop();
        return;

    case AtomicsAdd:
    case AtomicsAnd:
    case AtomicsCompareExchange:
    case AtomicsExchange:
    case AtomicsLoad:
    case AtomicsOr:
    case AtomicsStore:
    case AtomicsSub:
    case AtomicsXor: {
        unsigned numExtraArgs = numExtraAtomicsArgs(node->op());
        Edge storageEdge = graph.child(node, 2 + numExtraArgs);
        if (!storageEdge) {
            clobberTop();
            return;
        }
        read(TypedArrayProperties);
        read(MiscFields);
        write(TypedArrayProperties);
        return;
    }

    case Throw:
    case ThrowStaticError:
    case TailCall:
    case DirectTailCall:
    case TailCallVarargs:
    case TailCallForwardVarargs:
        read(World);
        write(SideState);
        return;
        
    case GetGetter:
        read(GetterSetter_getter);
        def(HeapLocation(GetterLoc, GetterSetter_getter, node->child1()), LazyNode(node));
        return;
        
    case GetSetter:
        read(GetterSetter_setter);
        def(HeapLocation(SetterLoc, GetterSetter_setter, node->child1()), LazyNode(node));
        return;
        
    case GetCallee:
        read(AbstractHeap(Stack, VirtualRegister(CallFrameSlot::callee)));
        def(HeapLocation(StackLoc, AbstractHeap(Stack, VirtualRegister(CallFrameSlot::callee))), LazyNode(node));
        return;

    case SetCallee:
        write(AbstractHeap(Stack, VirtualRegister(CallFrameSlot::callee)));
        return;
        
    case GetArgumentCountIncludingThis: {
        auto heap = AbstractHeap(Stack, remapOperand(node->argumentsInlineCallFrame(), VirtualRegister(CallFrameSlot::argumentCountIncludingThis)));
        read(heap);
        def(HeapLocation(StackPayloadLoc, heap), LazyNode(node));
        return;
    }

    case SetArgumentCountIncludingThis:
        write(AbstractHeap(Stack, VirtualRegister(CallFrameSlot::argumentCountIncludingThis)));
        return;
        
    case GetLocal:
        read(AbstractHeap(Stack, node->operand()));
        def(HeapLocation(StackLoc, AbstractHeap(Stack, node->operand())), LazyNode(node));
        return;
        
    case SetLocal:
        write(AbstractHeap(Stack, node->operand()));
        def(HeapLocation(StackLoc, AbstractHeap(Stack, node->operand())), LazyNode(node->child1().node()));
        return;
        
    case GetStack: {
        AbstractHeap heap(Stack, node->stackAccessData()->operand);
        read(heap);
        def(HeapLocation(StackLoc, heap), LazyNode(node));
        return;
    }
        
    case PutStack: {
        AbstractHeap heap(Stack, node->stackAccessData()->operand);
        write(heap);
        def(HeapLocation(StackLoc, heap), LazyNode(node->child1().node()));
        return;
    }
        
    case VarargsLength: {
        clobberTop();
        return;  
    }

    case LoadVarargs: {
        if (node->argumentsChild().useKind() != OtherUse)
            clobberTop();
        LoadVarargsData* data = node->loadVarargsData();
        write(AbstractHeap(Stack, data->count));
        for (unsigned i = data->limit; i--;)
            write(AbstractHeap(Stack, data->start + static_cast<int>(i)));
        return;
    }
        
    case ForwardVarargs: {
        // We could be way more precise here.
        read(Stack);
        
        LoadVarargsData* data = node->loadVarargsData();
        write(AbstractHeap(Stack, data->count));
        for (unsigned i = data->limit; i--;)
            write(AbstractHeap(Stack, data->start + static_cast<int>(i)));
        return;
    }

    case EnumeratorGetByVal: {
        clobberTop();
        return;
    }
        
    case GetByVal:
    case GetByValMegamorphic: {
        ArrayMode mode = node->arrayMode();
        LocationKind indexedPropertyLoc = indexedPropertyLocForResultType(node->result());
        // T3-jit-segmented-arraymode: when the segmented bit is set, the DFG
        // backend re-loads the tagged butterfly word directly (no GetButterfly
        // child), so this node itself reads JSObject_butterfly. Flag-off
        // byte-identical (the bit is never set without useJSThreads).
        if (mode.mayBeSegmentedButterfly())
            read(JSObject_butterfly);
        switch (mode.type()) {
        case Array::SelectUsingPredictions:
        case Array::Unprofiled:
        case Array::SelectUsingArguments:
            // Assume the worst since we don't have profiling yet.
            clobberTop();
            return;
            
        case Array::ForceExit:
            write(SideState);
            return;
            
        case Array::Generic:
        case Array::BigInt64Array:
        case Array::BigUint64Array:
            clobberTop();
            return;
            
        case Array::String:
            if (mode.isOutOfBounds()) {
                clobberTop();
                return;
            }
            // This appears to read nothing because it's only reading immutable data.
            def(PureValue(graph, node, mode.asWord()));
            return;
            
        case Array::DirectArguments:
            if (mode.isInBounds()) {
                read(DirectArgumentsProperties);
                def(HeapLocation(indexedPropertyLoc, DirectArgumentsProperties, graph.varArgChild(node, 0), graph.varArgChild(node, 1)), LazyNode(node));
                return;
            }
            clobberTop();
            return;
            
        case Array::ScopedArguments:
            read(ScopeProperties);
            def(HeapLocation(indexedPropertyLoc, ScopeProperties, graph.varArgChild(node, 0), graph.varArgChild(node, 1)), LazyNode(node));
            return;
            
        case Array::Int32:
            if (mode.isInBounds() || mode.isOutOfBoundsSaneChain()) {
                read(Butterfly_publicLength);
                read(IndexedInt32Properties);
                LocationKind kind = mode.isOutOfBoundsSaneChain() ? IndexedPropertyInt32OutOfBoundsSaneChainLoc : indexedPropertyLoc;
                def(HeapLocation(kind, IndexedInt32Properties, graph.varArgChild(node, 0), graph.varArgChild(node, 1)), LazyNode(node));
                return;
            }
            clobberTop();
            return;
            
        case Array::Double:
            if (mode.isInBounds() || mode.isOutOfBoundsSaneChain()) {
                read(Butterfly_publicLength);
                read(IndexedDoubleProperties);
                LocationKind kind;
                if (node->hasDoubleResult()) {
                    if (mode.isInBoundsSaneChain())
                        kind = IndexedPropertyDoubleSaneChainLoc;
                    else if (mode.isOutOfBoundsSaneChain())
                        kind = IndexedPropertyDoubleOutOfBoundsSaneChainLoc;
                    else
                        kind = IndexedPropertyDoubleLoc;
                } else {
                    ASSERT(mode.isOutOfBoundsSaneChain());
                    kind = IndexedPropertyDoubleOrOtherOutOfBoundsSaneChainLoc;
                }
                def(HeapLocation(kind, IndexedDoubleProperties, graph.varArgChild(node, 0), graph.varArgChild(node, 1)), LazyNode(node));
                return;
            }
            clobberTop();
            return;
            
        case Array::Contiguous:
            if (mode.isInBounds() || mode.isOutOfBoundsSaneChain()) {
                read(Butterfly_publicLength);
                read(IndexedContiguousProperties);
                def(HeapLocation(mode.isOutOfBoundsSaneChain() ? IndexedPropertyJSOutOfBoundsSaneChainLoc : indexedPropertyLoc, IndexedContiguousProperties, graph.varArgChild(node, 0), graph.varArgChild(node, 1)), LazyNode(node));
                return;
            }
            clobberTop();
            return;

        case Array::Undecided:
            def(PureValue(graph, node));
            return;
            
        case Array::ArrayStorage:
        case Array::SlowPutArrayStorage:
            if (mode.isInBounds()) {
                read(Butterfly_vectorLength);
                read(IndexedArrayStorageProperties);
                return;
            }
            clobberTop();
            return;
            
        case Array::Int8Array:
        case Array::Int16Array:
        case Array::Int32Array:
        case Array::Uint8Array:
        case Array::Uint8ClampedArray:
        case Array::Uint16Array:
        case Array::Uint32Array:
        case Array::Float16Array:
        case Array::Float32Array:
        case Array::Float64Array:
            // Even if we hit out-of-bounds, this is fine. TypedArray does not propagate access to its [[Prototype]] when out-of-bounds access happens.
            read(TypedArrayProperties);
            read(MiscFields);
            if (mode.mayBeResizableOrGrowableSharedTypedArray()) {
                write(MiscFields);
                write(TypedArrayProperties);
            } else {
                if (mode.isOutOfBounds())
                    indexedPropertyLoc = indexedPropertyLocToOutOfBoundsSaneChain(indexedPropertyLoc);
                def(HeapLocation(indexedPropertyLoc, TypedArrayProperties, graph.varArgChild(node, 0), graph.varArgChild(node, 1)), LazyNode(node));
            }
            return;
        // We should not get an AnyTypedArray in a GetByVal as AnyTypedArray is only created from intrinsics, which
        // are only added from Inline Caching a GetById.
        case Array::AnyTypedArray:
            DFG_CRASH(graph, node, "impossible array mode for get");
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
        return;
    }

    case MultiGetByVal: {
        ArrayMode mode = node->arrayMode();
        LocationKind indexedPropertyLoc = indexedPropertyLocForResultType(node->result());
        bool canUseCSE = true;
        for (unsigned i = 0; i < sizeof(ArrayModes) * CHAR_BIT; ++i) {
            ArrayModes oneArrayMode = 1ULL << i;
            if (node->arrayModes() & oneArrayMode) {
                switch (oneArrayMode) {
                case asArrayModesIgnoringTypedArrays(ArrayWithInt32): {
                    if (mode.isInBounds() || mode.isOutOfBoundsSaneChain()) {
                        read(Butterfly_publicLength);
                        read(IndexedInt32Properties);
                        break;
                    }
                    clobberTop();
                    break;
                }
                case asArrayModesIgnoringTypedArrays(ArrayWithDouble): {
                    if (mode.isInBounds() || mode.isOutOfBoundsSaneChain()) {
                        read(Butterfly_publicLength);
                        read(IndexedDoubleProperties);
                        break;
                    }
                    clobberTop();
                    break;
                }
                case asArrayModesIgnoringTypedArrays(ArrayWithContiguous): {
                    if (mode.isInBounds() || mode.isOutOfBoundsSaneChain()) {
                        read(Butterfly_publicLength);
                        read(IndexedContiguousProperties);
                        break;
                    }
                    clobberTop();
                    break;
                }
                case Int8ArrayMode:
                case Int16ArrayMode:
                case Int32ArrayMode:
                case Uint8ArrayMode:
                case Uint8ClampedArrayMode:
                case Float16ArrayMode:
                case Uint16ArrayMode:
                case Uint32ArrayMode:
                case Float32ArrayMode:
                case Float64ArrayMode:
                case BigInt64ArrayMode:
                case BigUint64ArrayMode:
                    // Even if we hit out-of-bounds, this is fine. TypedArray does not propagate access to its [[Prototype]] when out-of-bounds access happens.
                    read(TypedArrayProperties);
                    read(MiscFields);
                    if (mode.mayBeResizableOrGrowableSharedTypedArray()) {
                        canUseCSE = false;
                        write(MiscFields);
                        write(TypedArrayProperties);
                    }
                    break;
                default:
                    DFG_CRASH(graph, node, "impossible array mode for MultiGetByVal");
                    break;
                }
            }
        }
        if (!mode.isOutOfBounds() && canUseCSE)
            def(HeapLocation(indexedPropertyLoc, IndexedProperties, graph.child(node, 0).node(), LazyNode(graph.child(node, 1).node()), nullptr, std::bit_cast<void*>(static_cast<uintptr_t>(node->arrayModes()))), LazyNode(node));
        return;
    }

    case GetMyArgumentByVal:
    case GetMyArgumentByValOutOfBounds: {
        read(Stack);
        // FIXME: It would be trivial to have a def here.
        // https://bugs.webkit.org/show_bug.cgi?id=143077
        return;
    }

    case PutByValDirect:
    case PutByVal:
    case PutByValDirectResolved:
    case PutByValMegamorphic: {
        ArrayMode mode = node->arrayMode();
        // T3-jit-segmented-arraymode: see GetByVal above.
        if (mode.mayBeSegmentedButterfly())
            read(JSObject_butterfly);
        Node* base = graph.varArgChild(node, 0).node();
        Node* index = graph.varArgChild(node, 1).node();
        Node* value = graph.varArgChild(node, 2).node();
        LocationKind indexedPropertyLoc = indexedPropertyLocForResultType(node->result());

        switch (mode.modeForPut().type()) {
        case Array::SelectUsingPredictions:
        case Array::SelectUsingArguments:
        case Array::Unprofiled:
        case Array::Undecided:
            // Assume the worst since we don't have profiling yet.
            clobberTop();
            return;
            
        case Array::ForceExit:
            write(SideState);
            return;
            
        case Array::Generic:
        case Array::BigInt64Array:
        case Array::BigUint64Array:
            clobberTop();
            return;
            
        case Array::Int32:
            if (mode.isOutOfBounds()) {
                clobberTop();
                return;
            }
            read(Butterfly_publicLength);
            read(Butterfly_vectorLength);
            read(IndexedInt32Properties);
            write(IndexedInt32Properties);
            if (mode.mayStoreToHole())
                write(Butterfly_publicLength);
            def(HeapLocation(indexedPropertyLoc, IndexedInt32Properties, base, index), LazyNode(value));
            def(HeapLocation(IndexedPropertyInt32OutOfBoundsSaneChainLoc, IndexedInt32Properties, base, index), LazyNode(value));
            return;
            
        case Array::Double:
            if (mode.isOutOfBounds()) {
                clobberTop();
                return;
            }
            read(Butterfly_publicLength);
            read(Butterfly_vectorLength);
            read(IndexedDoubleProperties);
            write(IndexedDoubleProperties);
            if (mode.mayStoreToHole())
                write(Butterfly_publicLength);
            def(HeapLocation(IndexedPropertyDoubleLoc, IndexedDoubleProperties, base, index), LazyNode(value));
            def(HeapLocation(IndexedPropertyDoubleSaneChainLoc, IndexedDoubleProperties, base, index), LazyNode(value));
            def(HeapLocation(IndexedPropertyDoubleOutOfBoundsSaneChainLoc, IndexedDoubleProperties, base, index), LazyNode(value));
            return;
            
        case Array::Contiguous:
            if (mode.isOutOfBounds()) {
                clobberTop();
                return;
            }
            read(Butterfly_publicLength);
            read(Butterfly_vectorLength);
            read(IndexedContiguousProperties);
            write(IndexedContiguousProperties);
            if (mode.mayStoreToHole())
                write(Butterfly_publicLength);
            def(HeapLocation(indexedPropertyLoc, IndexedContiguousProperties, base, index), LazyNode(value));
            def(HeapLocation(IndexedPropertyJSOutOfBoundsSaneChainLoc, IndexedContiguousProperties, base, index), LazyNode(value));
            return;
            
        case Array::ArrayStorage:
            if (node->arrayMode().isOutOfBounds()) {
                clobberTop();
                return;
            }
            read(Butterfly_publicLength);
            read(Butterfly_vectorLength);
            read(IndexedArrayStorageProperties);
            write(IndexedArrayStorageProperties);
            if (mode.mayStoreToHole())
                write(Butterfly_publicLength);
            return;

        case Array::SlowPutArrayStorage:
            if (mode.mayStoreToHole()) {
                clobberTop();
                return;
            }
            read(Butterfly_publicLength);
            read(Butterfly_vectorLength);
            read(IndexedArrayStorageProperties);
            write(IndexedArrayStorageProperties);
            return;

        case Array::Int8Array:
        case Array::Int16Array:
        case Array::Int32Array:
        case Array::Uint8Array:
        case Array::Uint8ClampedArray:
        case Array::Uint16Array:
        case Array::Uint32Array:
        case Array::Float16Array:
        case Array::Float32Array:
        case Array::Float64Array:
            if (mode.mayBeResizableOrGrowableSharedTypedArray()) {
                read(TypedArrayProperties);
                read(MiscFields);
                write(TypedArrayProperties);
                write(MiscFields);
            } else {
                read(MiscFields);
                write(TypedArrayProperties);
                // FIXME: We can't def() anything here because these operations truncate their inputs.
                // https://bugs.webkit.org/show_bug.cgi?id=134737
            }
            return;
        case Array::AnyTypedArray:
        case Array::String:
        case Array::DirectArguments:
        case Array::ScopedArguments:
            DFG_CRASH(graph, node, "impossible array mode for put");
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
        return;
    }

    case MultiPutByVal: {
        ArrayMode mode = node->arrayMode();
        for (unsigned i = 0; i < sizeof(ArrayModes) * CHAR_BIT; ++i) {
            ArrayModes oneArrayMode = 1ULL << i;
            if (node->arrayModes() & oneArrayMode) {
                switch (oneArrayMode) {
                case asArrayModesIgnoringTypedArrays(ArrayWithInt32): {
                    if (mode.isOutOfBounds()) {
                        clobberTop();
                        break;
                    }
                    read(Butterfly_publicLength);
                    read(Butterfly_vectorLength);
                    read(IndexedInt32Properties);
                    write(IndexedInt32Properties);
                    if (mode.mayStoreToHole())
                        write(Butterfly_publicLength);
                    break;
                }
                case asArrayModesIgnoringTypedArrays(ArrayWithDouble): {
                    if (mode.isOutOfBounds()) {
                        clobberTop();
                        break;
                    }
                    read(Butterfly_publicLength);
                    read(Butterfly_vectorLength);
                    read(IndexedDoubleProperties);
                    write(IndexedDoubleProperties);
                    if (mode.mayStoreToHole())
                        write(Butterfly_publicLength);
                    break;
                }
                case asArrayModesIgnoringTypedArrays(ArrayWithContiguous): {
                    if (mode.isOutOfBounds()) {
                        clobberTop();
                        break;
                    }
                    read(Butterfly_publicLength);
                    read(Butterfly_vectorLength);
                    read(IndexedContiguousProperties);
                    write(IndexedContiguousProperties);
                    if (mode.mayStoreToHole())
                        write(Butterfly_publicLength);
                    break;
                }
                case Int8ArrayMode:
                case Int16ArrayMode:
                case Int32ArrayMode:
                case Uint8ArrayMode:
                case Uint8ClampedArrayMode:
                case Float16ArrayMode:
                case Uint16ArrayMode:
                case Uint32ArrayMode:
                case Float32ArrayMode:
                case Float64ArrayMode:
                case BigInt64ArrayMode:
                case BigUint64ArrayMode:
                    // Even if we hit out-of-bounds, this is fine. TypedArray does not propagate access to its [[Prototype]] when out-of-bounds access happens.
                    if (mode.mayBeResizableOrGrowableSharedTypedArray()) {
                        read(TypedArrayProperties);
                        read(MiscFields);
                        write(TypedArrayProperties);
                        write(MiscFields);
                    } else {
                        read(MiscFields);
                        write(TypedArrayProperties);
                    }
                    break;
                default:
                    DFG_CRASH(graph, node, "impossible array mode for MultiPutByVal");
                    break;
                }
            }
        }
        return;
    }

    case EnumeratorPutByVal: {
        clobberTop();
        return;
    }

    case CheckStructureOrEmpty:
    case CheckStructure:
        read(JSCell_structureID);
        return;

    case CheckPrivateBrand:
        read(JSCell_structureID);
        def(HeapLocation(CheckPrivateBrandLoc, JSCell_structureID, node->child1(), node->child2()), LazyNode(node));
        return;

    case SetPrivateBrand:
        read(JSCell_structureID);
        write(JSCell_structureID);
        return;

    case CheckArrayOrEmpty:
    case CheckArray:
        read(JSCell_indexingType);
        read(JSCell_structureID);
        return;

    case CheckDetached:
        read(MiscFields);
        return; 
        
    case CheckTypeInfoFlags:
        read(JSCell_typeInfoFlags);
        def(HeapLocation(CheckTypeInfoFlagsLoc, JSCell_typeInfoFlags, node->child1()), LazyNode(node));
        return;

    case HasStructureWithFlags:
        read(World);
        return;

    case ParseInt:
        // Note: We would have eliminated a ParseInt that has just a single child as an Int32Use inside fixup.
        if (node->child1().useKind() == StringUse || node->child1().useKind() == DoubleRepUse || node->child1().useKind() == Int32Use) {
            if (!node->child2() || node->child2().useKind() == Int32Use) {
                def(PureValue(node));
                return;
            }
        }

        clobberTop();
        return;

    case ToIntegerOrInfinity:
    case ToLength: {
        if (node->child1().useKind() == UntypedUse)
            clobberTop();
        else
            def(PureValue(node));
        return;
    }

    case OverridesHasInstance:
        read(JSCell_typeInfoFlags);
        def(HeapLocation(OverridesHasInstanceLoc, JSCell_typeInfoFlags, node->child1()), LazyNode(node));
        return;

    case PutStructure:
        read(JSObject_butterfly);
        write(JSCell_structureID);
        write(JSCell_typeInfoFlags);
        write(JSCell_indexingType);

        if (node->transition()->next->transitionKind() == TransitionKind::PropertyDeletion) {
            // We use this "delete fence" to model the proper aliasing of future stores.
            // Both in DFG and when we lower to B3, we model aliasing of properties by
            // property  name. In a world without delete, that also models {base, propertyOffset}.
            // However, with delete, we may reuse property offsets for different names.
            // Those potential stores that come after this delete won't properly model
            // that they are dependent on the prior name stores. For example, if we didn't model this,
            // it could give when doing things like store elimination, since we don't see
            // writes to the new field name as having dependencies on the old field name.
            // This node makes it so we properly model those dependencies.
            write(NamedProperties);
        }
            
        return;
        
    case AllocatePropertyStorage:
    case ReallocatePropertyStorage:
        read(HeapObjectCount);
        write(HeapObjectCount);
        return;
        
    case NukeStructureAndSetButterfly:
        write(JSObject_butterfly);
        write(JSCell_structureID);
        def(HeapLocation(ButterflyLoc, JSObject_butterfly, node->child1()), LazyNode(node->child2().node()));
        return;
        
    case GetButterfly:
        read(JSObject_butterfly);
        if (Options::useJSThreads()) [[unlikely]] {
            // SPEC-jit section 5.5 / Task 9: flag-on, GetButterfly emits the
            // read predicate (structureID for the ARM64 R7/F7 dependency,
            // indexing byte for the AS-rule SW test).
            read(JSCell_structureID);
            read(JSCell_indexingType);
        }
        def(HeapLocation(ButterflyLoc, JSObject_butterfly, node->child1()), LazyNode(node));
        return;

    case CheckJSCast:
    case CheckNotJSCast:
        def(PureValue(node, node->classInfo()));
        return;

    case CallDOMGetter: {
        DOMJIT::CallDOMGetterSnippet* snippet = node->callDOMGetterData()->snippet;
        if (!snippet) {
            clobberTop();
            return;
        }
        DOMJIT::Effect effect = snippet->effect;
        if (effect.domReads == DOMJIT::HeapRange::top())
            read(World);
        else {
            if (effect.domReads)
                read(AbstractHeap(DOMState, effect.domReads.rawRepresentation()));
            WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
            for (unsigned i = 0; i < 4; ++i) {
                if (effect.reads[i] == InvalidAbstractHeap)
                    break;
                read(effect.reads[i]);
            }
            WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
        }
        if (effect.domWrites == DOMJIT::HeapRange::top()) {
            if (Options::validateDFGClobberize())
                clobberTopFunctor();
             write(Heap);
        }
        else {
            if (effect.domWrites) 
                write(AbstractHeap(DOMState, effect.domWrites.rawRepresentation()));
            WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
            for (unsigned i = 0; i < 4; ++i) {
                if (effect.writes[i] == InvalidAbstractHeap)
                    break;
                write(effect.writes[i]);
            }
            WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
        }
        if (effect.def != DOMJIT::HeapRange::top()) {
            DOMJIT::HeapRange range = effect.def;
            if (range == DOMJIT::HeapRange::none())
                def(PureValue(node, std::bit_cast<uintptr_t>(node->callDOMGetterData()->customAccessorGetter)));
            else {
                // Def with heap location. We do not include "GlobalObject" for that since this information is included in the base node.
                // We only see the DOMJIT getter here. So just including "base" is ok.
                def(HeapLocation(DOMStateLoc, AbstractHeap(DOMState, range.rawRepresentation()), node->child1()), LazyNode(node));
            }
        }
        return;
    }

    case CallDOM: {
        const DOMJIT::Signature* signature = node->signature();
        DOMJIT::Effect effect = signature->effect;
        if (effect.domReads == DOMJIT::HeapRange::top())
            read(World);
        else {
            if (effect.domReads)
                read(AbstractHeap(DOMState, effect.domReads.rawRepresentation()));
            WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
            for (unsigned i = 0; i < 4; ++i) {
                if (effect.reads[i] == InvalidAbstractHeap)
                    break;
                read(effect.reads[i]);
            }
            WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
        }
        if (effect.domWrites == DOMJIT::HeapRange::top()) {
            if (Options::validateDFGClobberize())
                clobberTopFunctor();
             write(Heap);
        }
        else {
            if (effect.domWrites) 
                write(AbstractHeap(DOMState, effect.domWrites.rawRepresentation()));
            WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
            for (unsigned i = 0; i < 4; ++i) {
                if (effect.writes[i] == InvalidAbstractHeap)
                    break;
                write(effect.writes[i]);
            }
            WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
        }
#ifndef BUN_SKIP_FAILING_ASSERTIONS
        ASSERT_WITH_MESSAGE(effect.def == DOMJIT::HeapRange::top(), "Currently, we do not accept any def for CallDOM.");
#endif
        return;
    }

    case Arrayify:
    case ArrayifyToStructure:
        read(JSCell_structureID);
        read(JSCell_indexingType);
        read(JSObject_butterfly);
        write(JSCell_structureID);
        write(JSCell_indexingType);
        write(JSObject_butterfly);
        write(Watchpoint_fire);
        // Allocates the (ArrayStorage) butterfly: GC-parkable GIL-off
        // (AUDIT-checktraps P10c).
        return;
        
    case GetIndexedPropertyStorage:
        ASSERT(node->arrayMode().type() != Array::String);
        read(MiscFields);
        def(HeapLocation(IndexedPropertyStorageLoc, MiscFields, node->child1()), LazyNode(node));
        return;

    case ResolveRope:
        def(PureValue(node));
        return;

    case GetTypedArrayByteOffset: {
        ArrayMode mode = node->arrayMode();
        DFG_ASSERT(graph, node, mode.isSomeTypedArrayView() || mode.type() == Array::ForceExit);
        switch (mode.type()) {
        case Array::ForceExit:
            write(SideState);
            return;
        default:
            read(MiscFields);
            if (node->arrayMode().mayBeResizableOrGrowableSharedTypedArray())
                write(MiscFields);
            else
                def(HeapLocation(TypedArrayByteOffsetLoc, MiscFields, node->child1()), LazyNode(node));
            return;
        }
        return;
    }

    case GetTypedArrayByteOffsetAsInt52: {
        ArrayMode mode = node->arrayMode();
        DFG_ASSERT(graph, node, mode.isSomeTypedArrayView() || mode.type() == Array::ForceExit);
        switch (mode.type()) {
        case Array::ForceExit:
            write(SideState);
            return;
        default:
            read(MiscFields);
            if (node->arrayMode().mayBeResizableOrGrowableSharedTypedArray())
                write(MiscFields);
            else
                def(HeapLocation(TypedArrayByteOffsetInt52Loc, MiscFields, node->child1()), LazyNode(node));
            return;
        }
        return;
    }

    case GetWebAssemblyInstanceExports:
        def(PureValue(node));
        return;

    case GetPrototypeOf: {
        switch (node->child1().useKind()) {
        case ArrayUse:
        case FunctionUse:
        case FinalObjectUse:
            read(JSCell_structureID);
            read(JSObject_butterfly);
            read(NamedProperties); // Poly proto could load prototype from its slot.
            def(HeapLocation(PrototypeLoc, NamedProperties, node->child1()), LazyNode(node));
            return;
        default:
            clobberTop();
            return;
        }
    }
        
    case GetByOffset:
    case GetGetterSetterByOffset: {
        unsigned identifierNumber = node->storageAccessData().identifierNumber;
        AbstractHeap heap(NamedProperties, identifierNumber);
        read(heap);

        // Since LICM might break the uniqueness assumption of HeapLocation for
        // *byOffset nodes. Then, the HeapLocation constructor with an extra state
        // is introduced and applied in this phase in order to resolve the potential
        // HeapLocation collisions for *byteOffset nodes after LICM phase. Note
        // that the constructor with an extra state should be used only after LICM
        // since it might affect performance.
        auto location = node->hasDoubleResult() ? NamedPropertyDoubleLoc : NamedPropertyLoc;
        if (graph.m_planStage >= PlanStage::LICMAndLater)
            def(HeapLocation(location, heap, node->child2(), &node->storageAccessData()), LazyNode(node));
        else
            def(HeapLocation(location, heap, node->child2()), LazyNode(node));
        return;
    }

    case MultiGetByOffset: {
        read(JSCell_structureID);
        read(JSObject_butterfly);
        if (Options::useJSThreads()) [[unlikely]] {
            // SPEC-jit section 5.5 / Task 10: flag-on, the FTL lowering emits
            // the read predicate (indexing byte for the conservative AS-rule
            // SW test on prototype-base / MaybeArrayStorage cases).
            read(JSCell_indexingType);
        }
        AbstractHeap heap(NamedProperties, node->multiGetByOffsetData().identifierNumber);
        read(heap);
        auto location = node->hasDoubleResult() ? NamedPropertyDoubleLoc : NamedPropertyLoc;
        if (graph.m_planStage >= PlanStage::LICMAndLater)
            def(HeapLocation(location, heap, node->child1(), &node->multiGetByOffsetData()), LazyNode(node));
        else
            def(HeapLocation(location, heap, node->child1()), LazyNode(node));
        return;
    }
        
    case MultiPutByOffset: {
        read(JSCell_structureID);
        read(JSObject_butterfly);
        if (Options::useJSThreads()) [[unlikely]] {
            // SPEC-jit section 5.5 / Task 10: flag-on write predicate
            // (indexing byte for the AS-rule arm on MaybeArrayStorage plans).
            read(JSCell_indexingType);
        }
        AbstractHeap heap(NamedProperties, node->multiPutByOffsetData().identifierNumber);
        write(heap);
        if (node->multiPutByOffsetData().writesStructures())
            write(JSCell_structureID);
        if (node->multiPutByOffsetData().reallocatesStorage())
            write(JSObject_butterfly);
        // AUDIT-checktraps P10b/P10c: a transitioning or storage-reallocating
        // variant runs a runtime slow path that can (a) allocate property
        // storage from the GC heap — a GC-parkable allocation — and (b) park
        // in the JSObjectInlines transition-wait spin
        // (parkSitePollAndParkForStopTheWorld), whose rejoin carries no
        // invalidation point. Both park classes admit a §A.3 heap-fact
        // window completing over them, so those variants clobber heap facts
        // GIL-off via jsThreadsParkableSlowPathClobbersHeapFacts (the
        // pre-switch central clobber above). Non-transitioning,
        // non-reallocating variants are plain stores with no parkable slow
        // path and keep the precise model.
        auto location = node->child2().useKind() == DoubleRepUse ? NamedPropertyDoubleLoc : NamedPropertyLoc;
        if (graph.m_planStage >= PlanStage::LICMAndLater)
            def(HeapLocation(location, heap, node->child1(), &node->multiPutByOffsetData()), LazyNode(node->child2().node()));
        else
            def(HeapLocation(location, heap, node->child1()), LazyNode(node->child2().node()));
        return;
    }

    case MultiDeleteByOffset: {
        read(JSCell_structureID);
        read(JSObject_butterfly);
        if (Options::useJSThreads()) [[unlikely]] {
            // SPEC-jit section 5.5 / Task 10: flag-on write predicate
            // (indexing byte for the AS-rule arm on MaybeArrayStorage plans).
            read(JSCell_indexingType);
        }
        AbstractHeap heap(NamedProperties, node->multiDeleteByOffsetData().identifierNumber);
        write(heap);
        if (node->multiDeleteByOffsetData().writesStructures()) {
            write(JSCell_structureID);
            // See comment in PutStructure about why this is needed for proper
            // alias analysis.
            write(NamedProperties);
        }
        return;
    }
        
    case PutByOffset: {
        unsigned identifierNumber = node->storageAccessData().identifierNumber;
        AbstractHeap heap(NamedProperties, identifierNumber);
        write(heap);
        if (Options::useJSThreads()) [[unlikely]] {
            // SPEC-jit section 5.5 / Task 9: flag-on, out-of-line PutByOffset
            // re-loads the tagged butterfly from the base object (plus the
            // structureID for the ARM64 R7/F7 dependency and possibly the
            // indexing byte for the AS-rule test) and runs the frozen write
            // predicate in the same poll-free window as the store.
            read(JSObject_butterfly);
            read(JSCell_structureID);
            read(JSCell_indexingType);
        }
        auto location = node->child3().useKind() == DoubleRepUse ? NamedPropertyDoubleLoc : NamedPropertyLoc;
        if (graph.m_planStage >= PlanStage::LICMAndLater)
            def(HeapLocation(location, heap, node->child2(), &node->storageAccessData()), LazyNode(node->child3().node()));
        else
            def(HeapLocation(location, heap, node->child2()), LazyNode(node->child3().node()));
        return;
    }
        
    case GetArrayLength: {
        ArrayMode mode = node->arrayMode();
        switch (mode.type()) {
        case Array::Undecided:
        case Array::Int32:
        case Array::Double:
        case Array::Contiguous:
        case Array::ArrayStorage:
        case Array::SlowPutArrayStorage:
            // T3-jit-segmented-arraymode: when the segmented bit is set, the
            // DFG backend re-loads the tagged butterfly word directly (no
            // GetButterfly child), so this node itself reads JSObject_butterfly.
            // Flag-off byte-identical (the bit is never set).
            if (mode.mayBeSegmentedButterfly())
                read(JSObject_butterfly);
            read(Butterfly_publicLength);
            def(HeapLocation(ArrayLengthLoc, Butterfly_publicLength, node->child1()), LazyNode(node));
            return;
            
        case Array::String:
            def(PureValue(node, mode.asWord()));
            return;

        case Array::DirectArguments:
        case Array::ScopedArguments:
            read(MiscFields);
            def(HeapLocation(ArrayLengthLoc, MiscFields, node->child1()), LazyNode(node));
            return;

        case Array::ForceExit: {
            write(SideState);
            return;
        }

        default:
            DFG_ASSERT(graph, node, mode.isSomeTypedArrayView());
            read(MiscFields);
            if (mode.mayBeResizableOrGrowableSharedTypedArray())
                write(MiscFields);
            else
                def(HeapLocation(ArrayLengthLoc, MiscFields, node->child1()), LazyNode(node));
            return;
        }
    }

    case DataViewGetByteLength:
    case DataViewGetByteLengthAsInt52: {
        read(MiscFields);
        if (node->mayBeResizableOrGrowableSharedArrayBuffer())
            write(MiscFields);
        else {
            auto location = node->op() == DataViewGetByteLength ? DataViewByteLengthLoc : DataViewByteLengthAsInt52Loc;
            def(HeapLocation(location, MiscFields, node->child1()), LazyNode(node));
        }
        return;
    }

    case GetUndetachedTypeArrayLength: {
        ArrayMode mode = node->arrayMode();
        DFG_ASSERT(graph, node, mode.isSomeTypedArrayView());
        DFG_ASSERT(graph, node, !mode.mayBeResizableOrGrowableSharedTypedArray());
        mode = mode.withAction(Array::Action::Read); // Force action to Read to prevent incorrect optimizations in equality checks.
        def(PureValue(node, mode.asWord()));
        return;
    }

    case GetTypedArrayLengthAsInt52: {
        ArrayMode mode = node->arrayMode();
        DFG_ASSERT(graph, node, mode.isSomeTypedArrayView() || mode.type() == Array::ForceExit);
        switch (mode.type()) {
        case Array::ForceExit:
            write(SideState);
            return;
        default:
            read(MiscFields);
            if (mode.mayBeResizableOrGrowableSharedTypedArray())
                write(MiscFields);
            else
                def(HeapLocation(TypedArrayLengthInt52Loc, MiscFields, node->child1()), LazyNode(node));
            return;
        }
    }

    case GetVectorLength: {
        ArrayMode mode = node->arrayMode();
        switch (mode.type()) {
        case Array::ArrayStorage:
        case Array::SlowPutArrayStorage:
            read(Butterfly_vectorLength);
            def(HeapLocation(VectorLengthLoc, Butterfly_vectorLength, node->child1()), LazyNode(node));
            return;

        default:
            RELEASE_ASSERT_NOT_REACHED();
            return;
        }
    }
        
    case GetClosureVar: {
        auto location = node->hasDoubleResult() ? ClosureVariableDoubleLoc : ClosureVariableLoc;
        read(AbstractHeap(ScopeProperties, node->scopeOffset().offset()));
        def(HeapLocation(location, AbstractHeap(ScopeProperties, node->scopeOffset().offset()), node->child1()), LazyNode(node));
        return;
    }
        
    case PutClosureVar: {
        auto location = node->child2().useKind() == DoubleRepUse ? ClosureVariableDoubleLoc : ClosureVariableLoc;
        write(AbstractHeap(ScopeProperties, node->scopeOffset().offset()));
        def(HeapLocation(location, AbstractHeap(ScopeProperties, node->scopeOffset().offset()), node->child1()), LazyNode(node->child2().node()));
        return;
    }

    case GetInternalField: {
        AbstractHeap heap(JSInternalFields, node->internalFieldIndex());
        read(heap);
        def(HeapLocation(InternalFieldObjectLoc, heap, node->child1()), LazyNode(node));
        return;
    }

    case PutInternalField: {
        AbstractHeap heap(JSInternalFields, node->internalFieldIndex());
        write(heap);
        def(HeapLocation(InternalFieldObjectLoc, heap, node->child1()), LazyNode(node->child2().node()));
        return;
    }

    case GetRegExpObjectLastIndex:
        read(RegExpObject_lastIndex);
        def(HeapLocation(RegExpObjectLastIndexLoc, RegExpObject_lastIndex, node->child1()), LazyNode(node));
        return;

    case SetRegExpObjectLastIndex:
        write(RegExpObject_lastIndex);
        def(HeapLocation(RegExpObjectLastIndexLoc, RegExpObject_lastIndex, node->child1()), LazyNode(node->child2().node()));
        return;

    case RecordRegExpCachedResult:
        write(RegExpState);
        return;
        
    case GetFromArguments: {
        AbstractHeap heap(DirectArgumentsProperties, node->capturedArgumentsOffset().offset());
        read(heap);
        def(HeapLocation(DirectArgumentsLoc, heap, node->child1()), LazyNode(node));
        return;
    }
        
    case PutToArguments: {
        AbstractHeap heap(DirectArgumentsProperties, node->capturedArgumentsOffset().offset());
        write(heap);
        def(HeapLocation(DirectArgumentsLoc, heap, node->child1()), LazyNode(node->child2().node()));
        return;
    }

    case GetArgument: {
        read(Stack);
        // FIXME: It would be trivial to have a def here.
        // https://bugs.webkit.org/show_bug.cgi?id=143077
        return;
    }
        
    case GetGlobalVar:
    case GetGlobalLexicalVariable: {
        auto location = node->hasDoubleResult() ? GlobalVariableDoubleLoc : GlobalVariableLoc;
        read(AbstractHeap(Absolute, node->variablePointer()));
        def(HeapLocation(location, AbstractHeap(Absolute, node->variablePointer())), LazyNode(node));
        return;
    }
        
    case PutGlobalVariable: {
        write(AbstractHeap(Absolute, node->variablePointer()));
        auto location = node->child2().useKind() == DoubleRepUse ? GlobalVariableDoubleLoc : GlobalVariableLoc;
        def(HeapLocation(location, AbstractHeap(Absolute, node->variablePointer())), LazyNode(node->child2().node()));
        return;
    }

    case NewArrayWithSpecies:
        clobberTop();
        return;

    case NewArrayWithSize:
    case NewArrayWithSizeAndStructure:
        read(HeapObjectCount);
        write(HeapObjectCount);
        return;

    case NewButterflyWithSize:
        read(HeapObjectCount);
        write(HeapObjectCount);
        // FIXME: In this phase we say the Array is where the length of the array is def'd but this differs from ObjectAllocationSinking.
        return;

    case PhantomNewArrayWithButterfly:
    case PhantomNewButterflyWithSize: {
        // No code, no park site (see jsThreadsParkableSlowPathClobbersHeapFacts).
        read(HeapObjectCount);
        write(HeapObjectCount);
        // FIXME: In this phase we say the Array is where the length of the array is def'd but this differs from ObjectAllocationSinking.
        return;
    }

    case GetCellButterflySlot:
        read(IndexedContiguousProperties);
        return;

    case PutCellButterflySlot:
        write(IndexedContiguousProperties);
        return;

    case ArraySortCompact: {
        AbstractHeap sourceHeap;
        switch (node->arrayMode().type()) {
        case Array::Int32:
            sourceHeap = IndexedInt32Properties;
            break;
        case Array::Contiguous:
            sourceHeap = IndexedContiguousProperties;
            break;
        default:
            DFG_CRASH(graph, node, "Bad array mode for ArraySortCompact");
            return;
        }
        read(JSObject_butterfly);
        if (Options::useJSThreads()) [[unlikely]]
            read(JSCell_structureID); // SPEC-jit section 5.5 / Task 10 (ARM64 R7/F7 dependency)
        read(Butterfly_publicLength);
        read(sourceHeap);
        read(HeapObjectCount);
        write(HeapObjectCount);
        write(IndexedContiguousProperties);
        return;
    }

    case ArraySortCommit: {
        AbstractHeap targetHeap;
        switch (node->arrayMode().type()) {
        case Array::Int32:
            targetHeap = IndexedInt32Properties;
            break;
        case Array::Contiguous:
            targetHeap = IndexedContiguousProperties;
            break;
        default:
            DFG_CRASH(graph, node, "Bad array mode for ArraySortCommit");
            return;
        }
        read(JSObject_butterfly);
        if (Options::useJSThreads()) [[unlikely]]
            read(JSCell_structureID); // SPEC-jit section 5.5 / Task 10 (ARM64 R7/F7 dependency)
        read(Butterfly_publicLength);
        read(IndexedContiguousProperties);
        write(IndexedContiguousProperties);
        write(targetHeap);
        return;
    }

    case MaterializeNewArrayWithButterfly:
        read(HeapObjectCount);
        write(HeapObjectCount);
        def(HeapLocation(ArrayLengthLoc, Butterfly_publicLength, node), LazyNode(graph.varArgChild(node, 0).node()));
        return;

    case NewArrayWithButterfly:
        read(HeapObjectCount);
        write(HeapObjectCount);
        def(HeapLocation(ArrayLengthLoc, Butterfly_publicLength, node), LazyNode(node->child1().node()));
        return;

    case NewTypedArray:
    case NewTypedArrayBuffer:
        switch (node->child1().useKind()) {
        case Int32Use:
        case Int52RepUse:
            read(HeapObjectCount);
            write(HeapObjectCount);
            return;
        case UntypedUse:
            clobberTop();
            return;
        default:
            DFG_CRASH(graph, node, "Bad use kind");
        }
        break;

    case NewArrayWithSpread: {
        read(HeapObjectCount);
        // This appears to read nothing because it's only reading immutable butterfly data.
        for (unsigned i = 0; i < node->numChildren(); i++) {
            Node* child = graph.varArgChild(node, i).node();
            if (child->op() == PhantomSpread) {
                read(Stack);
                break;
            }
        }
        write(HeapObjectCount);
        return;
    }

    case Spread: {
        if (node->child1()->op() == PhantomNewArrayBuffer) {
            read(MiscFields);
            return;
        }

        if (node->child1()->op() == PhantomCreateRest) {
            read(Stack);
            write(HeapObjectCount);
            return;
        }

        clobberTop();
        return;
    }

    case NewArray: {
        read(HeapObjectCount);
        write(HeapObjectCount);

        unsigned numElements = node->numChildren();

        def(HeapLocation(ArrayLengthLoc, Butterfly_publicLength, node),
            LazyNode(graph.freeze(jsNumber(numElements))));

        if (!numElements)
            return;

        AbstractHeap heap;
        LocationKind indexedPropertyLoc;
        switch (node->indexingType()) {
        case ALL_DOUBLE_INDEXING_TYPES:
            heap = IndexedDoubleProperties;
            indexedPropertyLoc = IndexedPropertyDoubleLoc;
            break;

        case ALL_INT32_INDEXING_TYPES:
            heap = IndexedInt32Properties;
            indexedPropertyLoc = IndexedPropertyJSLoc;
            break;

        case ALL_CONTIGUOUS_INDEXING_TYPES:
            heap = IndexedContiguousProperties;
            indexedPropertyLoc = IndexedPropertyJSLoc;
            break;

        default:
            return;
        }

        if (numElements < graph.m_uint32ValuesInUse.size()) {
            for (unsigned operandIdx = 0; operandIdx < numElements; ++operandIdx) {
                Edge use = graph.m_varArgChildren[node->firstChild() + operandIdx];
                def(HeapLocation(indexedPropertyLoc, heap, node, LazyNode(graph.freeze(jsNumber(operandIdx)))),
                    LazyNode(use.node()));
            }
        } else {
            for (uint32_t operandIdx : graph.m_uint32ValuesInUse) {
                if (operandIdx >= numElements)
                    continue;
                Edge use = graph.m_varArgChildren[node->firstChild() + operandIdx];
                // operandIdx comes from graph.m_uint32ValuesInUse and thus is guaranteed to be already frozen
                def(HeapLocation(indexedPropertyLoc, heap, node, LazyNode(graph.freeze(jsNumber(operandIdx)))),
                    LazyNode(use.node()));
            }
        }
        return;
    }

    case NewArrayBuffer: {
        read(HeapObjectCount);
        write(HeapObjectCount);

        auto* array = node->castOperand<JSCellButterfly*>();
        unsigned numElements = array->length();
        def(HeapLocation(ArrayLengthLoc, Butterfly_publicLength, node),
            LazyNode(graph.freeze(jsNumber(numElements))));

        AbstractHeap heap;
        LocationKind indexedPropertyLoc;
        NodeType op = JSConstant;
        switch (node->indexingType()) {
        case ALL_DOUBLE_INDEXING_TYPES:
            heap = IndexedDoubleProperties;
            indexedPropertyLoc = IndexedPropertyDoubleLoc;
            op = DoubleConstant;
            break;

        case ALL_INT32_INDEXING_TYPES:
            heap = IndexedInt32Properties;
            indexedPropertyLoc = IndexedPropertyJSLoc;
            break;

        case ALL_CONTIGUOUS_INDEXING_TYPES:
            heap = IndexedContiguousProperties;
            indexedPropertyLoc = IndexedPropertyJSLoc;
            break;

        default:
            return;
        }

        if (numElements < graph.m_uint32ValuesInUse.size()) {
            for (unsigned index = 0; index < numElements; ++index) {
                def(HeapLocation(indexedPropertyLoc, heap, node, LazyNode(graph.freeze(jsNumber(index)))),
                    LazyNode(graph.freeze(array->get(index)), op));
            }
        } else {
            Vector<uint32_t> possibleIndices;
            for (uint32_t index : graph.m_uint32ValuesInUse) {
                if (index >= numElements)
                    continue;
                possibleIndices.append(index);
            }
            for (uint32_t index : possibleIndices) {
                def(HeapLocation(indexedPropertyLoc, heap, node, LazyNode(graph.freeze(jsNumber(index)))),
                    LazyNode(graph.freeze(array->get(index)), op));
            }
        }
        return;
    }

    case CreateRest: {
        if (!graph.isWatchingHavingABadTimeWatchpoint(node)) {
            // This means we're already having a bad time.
            clobberTop();
            return;
        }
        read(Stack);
        read(HeapObjectCount);
        write(HeapObjectCount);
        return;
    }

    case ObjectAssign: {
        clobberTop();
        return;
    }

    case ObjectCreate:
        read(HeapObjectCount);
        write(HeapObjectCount);
        write(JSCell_structureID); // prototype object can be transitioned.
        write(Watchpoint_fire);
        if (node->child1().useKind() == UntypedUse)
            write(SideState);
        return;

    case NewSymbol:
        if (!node->child1() || node->child1().useKind() == StringUse) {
            read(HeapObjectCount);
            write(HeapObjectCount);
        } else
            clobberTop();
        return;

    case NewRegExpUntyped:
        if (node->child1().useKind() == StringUse && node->child2().useKind() == StringUse) {
            // SyntaxError may happen.
            read(World);
            write(SideState);
            write(HeapObjectCount);
        } else
            clobberTop();
        return;

    case NewObject:
    case NewInternalFieldObject:
    case NewPromise:
    case NewRegExp:
    case NewStringObject:
    case NewMap:
    case NewSet:
    case NewWeakMap:
    case NewWeakSet:
    case MaterializeNewObject:
    case MaterializeNewInternalFieldObject:
    case MaterializeCreateActivation:
        read(HeapObjectCount);
        write(HeapObjectCount);
        return;

    case PhantomNewObject:
    case PhantomNewFunction:
    case PhantomNewGeneratorFunction:
    case PhantomNewAsyncFunction:
    case PhantomNewAsyncGeneratorFunction:
    case PhantomNewInternalFieldObject:
    case PhantomNewPromise:
    case PhantomCreateActivation:
    case PhantomNewRegExp:
        // No code, no park site (see jsThreadsParkableSlowPathClobbersHeapFacts).
        read(HeapObjectCount);
        write(HeapObjectCount);
        return;

    case NewFunction:
    case NewGeneratorFunction:
    case NewAsyncGeneratorFunction:
    case NewAsyncFunction:
        if (node->castOperand<FunctionExecutable*>()->singleton().isStillValid())
            write(Watchpoint_fire);
        read(HeapObjectCount);
        write(HeapObjectCount);
        return;

    case NewBoundFunction:
        read(HeapObjectCount);
        write(HeapObjectCount);
        return;

    case RegExpSearch:
    case RegExpExec:
    case RegExpTest:
    case RegExpTestInline:
    case RegExpSplitFast:
    case RegExpStringIteratorNext:
        // Even if we've proven known input types as RegExpObject and String,
        // accessing lastIndex is effectful if it's a global regexp.
        clobberTop();
        return;

    case RegExpMatchFast:
        read(RegExpState);
        read(RegExpObject_lastIndex);
        write(RegExpState);
        write(RegExpObject_lastIndex);
        return;

    case RegExpExecSticky:
        read(RegExpState);
        read(RegExpObject_lastIndex);
        write(RegExpState);
        write(RegExpObject_lastIndex);
        return;

    case RegExpExecNonGlobalOrSticky:
    case RegExpMatchFastGlobal:
        read(RegExpState);
        write(RegExpState);
        return;

    case StringReplace:
    case StringReplaceAll:
    case StringReplaceRegExp:
        if (node->child1().useKind() == StringUse
            && node->child2().useKind() == RegExpObjectUse
            && node->child3().useKind() == StringUse) {
            read(RegExpState);
            read(RegExpObject_lastIndex);
            write(RegExpState);
            write(RegExpObject_lastIndex);
            return;
        }
        clobberTop();
        return;

    case StringSplit:
    case StringMatch:
    case StringSearch:
        clobberTop();
        return;

    case StringReplaceString:
        if (node->child3().useKind() == StringUse)
            return;
        clobberTop();
        return;

    case StringAt:
        // String.prototype.at returns a string when in bounds and undefined when OOB. This is
        // unlike charAt, which always returns a string. Include arrayMode to prevent CSE across
        // modes.
        def(PureValue(node, node->arrayMode().asWord()));
        return;
    case StringCharAt:
        def(PureValue(node));
        return;

    case StringIteratorNext:
    case StringIteratorNextWithUndefined:
        // Reads only immutable string contents and allocates the result string, so it is pure
        // with respect to the heap. It never touches the iterator object, so the
        // GetInternalField/PutInternalField pair around it stays visible to
        // ObjectAllocationSinking. Unlike other pure nodes we do not def(PureValue) here: this is
        // a tuple node and CSE's value-replacement would corrupt ExtractFromTuple references.
        return;

    case CompareBelow:
    case CompareBelowEq:
        def(PureValue(node));
        return;
        
    case CompareEq:
    case CompareLess:
    case CompareLessEq:
    case CompareGreater:
    case CompareGreaterEq:
        if (node->isBinaryUseKind(StringUse)) {
            read(HeapObjectCount);
            write(HeapObjectCount);
            return;
        }

        if (node->isBinaryUseKind(UntypedUse)) {
            clobberTop();
            return;
        }

        // CompareEq is realm-dependent when MasqueradesAsUndefined is involved.
        // Including the globalObject ensures nodes from different realms are not folded by CSE.
        if (node->op() == CompareEq) {
            def(PureValue(node, graph.globalObjectFor(node->origin.semantic)));
            return;
        }

        def(PureValue(node));
        return;

    case ToString:
    case CallStringConstructor:
        switch (node->child1().useKind()) {
        // KnownCellUse / KnownInt32Use arise from FixupPhase's final
        // check-hoisting pass (fixupChecksInBlock): when this node sits in an
        // ExitInvalid stretch, the CellUse/Int32Use type check is hoisted to
        // the closest exitOK point and the edge is strengthened to its Known
        // form. Semantics are unchanged — the check still runs, just earlier —
        // so each Known kind is treated exactly like its checked counterpart.
        case CellUse:
        case KnownCellUse:
        case UntypedUse:
            clobberTop();
            return;

        case KnownPrimitiveUse:
            write(SideState);
            return;

        case StringObjectUse:
        case StringOrStringObjectUse:
            // These two StringObjectUse's are pure because if we emit this node with either
            // of these UseKinds, we'll first emit a StructureCheck ensuring that we're the
            // original String or StringObject structure. Therefore, we don't have an overridden
            // valueOf, etc.

        case StringOrOtherUse:
        case Int32Use:
        case KnownInt32Use:
        case Int52RepUse:
        case DoubleRepUse:
        case NotCellUse:
            def(PureValue(node));
            return;
            
        default:
            RELEASE_ASSERT_NOT_REACHED();
            return;
        }

    case FunctionToString:
        def(PureValue(node));
        return;

    case FunctionBind:
        clobberTop(); // Slow path can clobber top.
        return;
        
    case CountExecution:
    case SuperSamplerBegin:
    case SuperSamplerEnd:
        read(InternalState);
        write(InternalState);
        return;
        
    case LogShadowChickenPrologue:
    case LogShadowChickenTail:
        write(SideState);
        return;

    case MapHash:
        def(PureValue(node));
        return;

    case NormalizeMapKey:
        def(PureValue(node));
        return;

    case MapGet: {
        Edge& mapEdge = node->child1();
        Edge& keyEdge = node->child2();
        Edge& hashEdge = node->child3();
        AbstractHeapKind heap = (mapEdge.useKind() == MapObjectUse) ? JSMapFields : JSSetFields;
        read(heap);
        def(HeapLocation(MapEntryKeyLoc, heap, mapEdge, keyEdge, hashEdge), LazyNode(node));
        return;
    }
    case LoadMapValue: {
        Edge& keySlotEdge = node->child1();
        AbstractHeapKind heap = JSMapFields;
        read(heap);
        def(HeapLocation(LoadMapValueLoc, heap, keySlotEdge), LazyNode(node));
        return;
    }

    case MapIteratorNext: {
        Edge& iteratedObjectEdge = node->child2();
        AbstractHeapKind ownerHeap = (iteratedObjectEdge.useKind() == MapObjectUse) ? JSMapFields : JSSetFields;
        read(ownerHeap);
        // We do not def here as it is tuple result.
        return;
    }
    case MapIteratorKey: {
        Edge& storageEdge = node->child1();
        Edge& entryEdge = node->child2();
        AbstractHeapKind heap = (node->bucketOwnerType() == BucketOwnerType::Map) ? JSMapFields : JSSetFields;
        read(heap);
        def(HeapLocation(MapIteratorKeyLoc, heap, storageEdge, entryEdge), LazyNode(node));
        return;
    }
    case MapIteratorValue: {
        Edge& storageEdge = node->child1();
        Edge& entryEdge = node->child2();
        read(JSMapFields);
        def(HeapLocation(MapIteratorValueLoc, JSMapFields, storageEdge, entryEdge), LazyNode(node));
        return;
    }

    case MapStorage: {
        Edge& mapEdge = node->child1();
        AbstractHeapKind heap = (mapEdge.useKind() == MapObjectUse) ? JSMapFields : JSSetFields;
        read(heap);
        def(HeapLocation(MapStorageLoc, heap, mapEdge, std::bit_cast<void*>(static_cast<intptr_t>(1))), LazyNode(node));
        return;
    }

    case MapStorageOrSentinel: {
        Edge& mapEdge = node->child1();
        AbstractHeapKind heap = (mapEdge.useKind() == MapObjectUse) ? JSMapFields : JSSetFields;
        read(heap);
        def(HeapLocation(MapStorageLoc, heap, mapEdge), LazyNode(node));
        return;
    }
    case MapIterationNext: {
        Edge& mapEdge = node->child1();
        Edge& entryEdge = node->child2();
        AbstractHeapKind heap = (node->bucketOwnerType() == BucketOwnerType::Map) ? JSMapFields : JSSetFields;
        read(heap);
        write(heap);
        def(HeapLocation(MapIterationNextLoc, heap, mapEdge, entryEdge), LazyNode(node));
        return;
    }
    case MapIterationEntry: {
        Edge& mapEdge = node->child1();
        AbstractHeapKind heap = (node->bucketOwnerType() == BucketOwnerType::Map) ? JSMapFields : JSSetFields;
        read(heap);
        def(HeapLocation(MapIterationEntryLoc, heap, mapEdge), LazyNode(node));
        return;
    }
    case MapIterationEntryKey: {
        Edge& mapEdge = node->child1();
        AbstractHeapKind heap = (node->bucketOwnerType() == BucketOwnerType::Map) ? JSMapFields : JSSetFields;
        read(heap);
        def(HeapLocation(MapIterationEntryKeyLoc, heap, mapEdge), LazyNode(node));
        return;
    }
    case MapIterationEntryValue: {
        Edge& mapEdge = node->child1();
        AbstractHeapKind heap = (node->bucketOwnerType() == BucketOwnerType::Map) ? JSMapFields : JSSetFields;
        read(heap);
        def(HeapLocation(MapIterationEntryValueLoc, heap, mapEdge), LazyNode(node));
        return;
    }

    case WeakMapGet: {
        Edge& mapEdge = node->child1();
        Edge& keyEdge = node->child2();
        AbstractHeapKind heap = (mapEdge.useKind() == WeakMapObjectUse) ? JSWeakMapFields : JSWeakSetFields;
        read(heap);
        def(HeapLocation(WeakMapGetLoc, heap, mapEdge, keyEdge), LazyNode(node));
        return;
    }

    case MapOrSetSize: {
        Edge& mapOrSetEdge = node->child1();
        AbstractHeapKind heap = (mapOrSetEdge.useKind() == MapObjectUse) ? JSMapFields : JSSetFields;
        read(heap);
        def(HeapLocation(MapOrSetSizeLoc, heap, mapOrSetEdge), LazyNode(node));
        return;
    }

    case GetRegExpFlag: {
        read(MiscFields);
        def(HeapLocation(GetRegExpFlagLoc, MiscFields, node->child1(), std::bit_cast<void*>(static_cast<uintptr_t>(node->regExpFlag()))), LazyNode(node));
        return;
    }

    case SetAdd: {
        Edge& mapEdge = node->child1();
        Edge& keyEdge = node->child2();
        write(JSSetFields);
        // Allocates/rehashes hash-map storage cells: GC-parkable GIL-off
        // (AUDIT-checktraps P10c). Same for MapSet/MapOrSetDelete/
        // WeakSetAdd/WeakMapSet below.
        def(HeapLocation(MapEntryValueLoc, JSSetFields, mapEdge, keyEdge), LazyNode(node));
        return;
    }

    case MapSet: {
        Edge& mapEdge = graph.varArgChild(node, 0);
        Edge& keyEdge = graph.varArgChild(node, 1);
        write(JSMapFields);
        def(HeapLocation(MapEntryValueLoc, JSMapFields, mapEdge, keyEdge), LazyNode(node));
        return;
    }

    case MapOrSetDelete: {
        Edge& mapEdge = node->child1();
        AbstractHeapKind heap = (mapEdge.useKind() == MapObjectUse) ? JSMapFields : JSSetFields;
        write(heap);
        return;
    }

    case WeakSetAdd: {
        Edge& mapEdge = node->child1();
        Edge& keyEdge = node->child2();
        if (keyEdge.useKind() != ObjectUse) {
            read(World);
            write(SideState);
        }
        write(JSWeakSetFields);
        def(HeapLocation(WeakMapGetLoc, JSWeakSetFields, mapEdge, keyEdge), LazyNode(keyEdge.node()));
        return;
    }

    case WeakMapSet: {
        Edge& mapEdge = graph.varArgChild(node, 0);
        Edge& keyEdge = graph.varArgChild(node, 1);
        Edge& valueEdge = graph.varArgChild(node, 2);
        if (keyEdge.useKind() != ObjectUse) {
            read(World);
            write(SideState);
        }
        write(JSWeakMapFields);
        def(HeapLocation(WeakMapGetLoc, JSWeakMapFields, mapEdge, keyEdge), LazyNode(valueEdge.node()));
        return;
    }

    case ExtractValueFromWeakMapGet:
        def(PureValue(node));
        return;

    case StringSlice:
    case StringSubstring:
    case StringSubstr:
        def(PureValue(node));
        return;

    case ToUpperCase:
    case ToLowerCase:
        def(PureValue(node));
        return;

    case StringTrim:
        def(PureValue(node, static_cast<uint64_t>(node->intrinsic())));
        return;

    case NumberToStringWithValidRadixConstant:
        def(PureValue(node, node->validRadixConstant()));
        return;

    case DateGetTime:
    case DateGetInt32OrNaN: {
        read(JSDateFields);
        def(HeapLocation(DateFieldLoc, AbstractHeap(JSDateFields, static_cast<uint64_t>(node->intrinsic())), node->child1()), LazyNode(node));
        return;
    }

    case DateSetTime: {
        write(JSDateFields);
        return;
    }

    case DataViewGetFloat:
    case DataViewGetInt: {
        read(MiscFields);
        read(TypedArrayProperties);
        if (node->dataViewData().isResizable) {
            write(MiscFields);
            write(TypedArrayProperties);
        } else {
            LocationKind indexedPropertyLoc = indexedPropertyLocToOutOfBoundsSaneChain(indexedPropertyLocForResultType(node->result()));
            def(HeapLocation(indexedPropertyLoc, AbstractHeap(TypedArrayProperties, node->dataViewData().asQuadWord), node->child1(), node->child2(), node->child3()), LazyNode(node));
        }
        return;
    }

    case DataViewSet: {
        read(MiscFields);
        read(TypedArrayProperties);
        if (node->dataViewData().isResizable)
            write(MiscFields);
        write(TypedArrayProperties);
        return;
    }

    case BufferReadInt:
    case BufferReadFloat: {
        if (node->arrayMode().type() == Array::ForceExit) {
            write(SideState);
            return;
        }
        DataViewData data = node->bufferAccessData();
        read(MiscFields);
        read(TypedArrayProperties);
        if (node->arrayMode().mayBeResizableOrGrowableSharedTypedArray()) {
            write(MiscFields);
            write(TypedArrayProperties);
        } else
            def(HeapLocation(indexedPropertyLocForResultType(node->result()), AbstractHeap(TypedArrayProperties, data.asQuadWord), graph.varArgChild(node, 0), graph.varArgChild(node, 1)), LazyNode(node));
        return;
    }

    case BufferWrite: {
        if (node->arrayMode().type() == Array::ForceExit) {
            write(SideState);
            return;
        }
        read(MiscFields);
        if (node->arrayMode().mayBeResizableOrGrowableSharedTypedArray()) {
            read(TypedArrayProperties);
            write(MiscFields);
        }
        write(TypedArrayProperties);
        return;
    }

    case ResolvePromiseFirstResolving:
    case RejectPromiseFirstResolving:
    case FulfillPromiseFirstResolving:
        clobberTop();
        return;

    case NewResolvedPromise:
        if (node->isResolvedValueKnownNonThenable()) {
            read(HeapObjectCount);
            write(HeapObjectCount);
            return;
        }
        clobberTop();
        return;

    case NewRejectedPromise:
        clobberTop();
        return;

    case PromiseResolve:
    case PromiseReject:
    case PromiseThen:
    case PerformPromiseThen:
    case PerformPromiseThenOneHandler:
        clobberTop();
        return;

    case LastNodeType:
        RELEASE_ASSERT_NOT_REACHED();
        return;
    }
    
    DFG_CRASH(graph, node, toCString("Unrecognized node type: ", Graph::opName(node->op())).data());
}

class NoOpClobberize {
public:
    NoOpClobberize() { }
    template<typename... T>
    void operator()(T...) const { }
};

class CheckClobberize {
public:
    CheckClobberize()
        : m_result(false)
    {
    }
    
    template<typename... T>
    void operator()(T...) const { m_result = true; }
    
    bool result() const { return m_result; }
    
private:
    mutable bool m_result;
};

bool doesWrites(Graph&, Node*);

class AbstractHeapOverlaps {
public:
    AbstractHeapOverlaps(AbstractHeap heap)
        : m_heap(heap)
        , m_result(false)
    {
    }
    
    void operator()(AbstractHeap otherHeap) const
    {
        if (m_result)
            return;
        m_result = m_heap.overlaps(otherHeap);
    }
    
    bool result() const { return m_result; }

private:
    AbstractHeap m_heap;
    mutable bool m_result;
};

bool accessesOverlap(Graph&, Node*, AbstractHeap);
bool writesOverlap(Graph&, Node*, AbstractHeap);

bool clobbersHeap(Graph&, Node*);

// SPEC-jit I14 + I21(b) Task-13 lint (Tier-B B3 / MC-JIT S2). Walks the
// finalized DFG/FTL graph just before backend codegen and asserts:
//   (a) I14: every node that dereferences a butterfly storage pointer takes
//       its storage edge from a tag-masking (or tag-zero-by-construction)
//       producer;
//   (b) I21(b) poll-placement: no GetButterfly result is consumed across a
//       JSObject_butterfly-clobbering boundary (CheckTraps poll, parkable
//       slow path, or any explicit butterfly write). With CheckTraps' GIL-off
//       clobberize entry above writing JSObject_butterfly, CSE/LICM enforce
//       (b) by construction; the lint is a regression guard so a future
//       de-jank that re-widens the poll's preserved-heap set is caught
//       immediately under JSC_validateButterflyTagDiscipline=1.
// Forward "available expressions" dataflow over GetButterfly defs.
// Flag-off: tag is always zero, function is a no-op (byte-identical LAW).
// Defined in dfg/DFGSpeculativeJIT.cpp; called from both backends just
// before codegen (SpeculativeJIT::compileBody / FTL LowerDFGToB3::lower).
void validateButterflyTagDisciplineForGraph(Graph&);

// We would have used bind() for these, but because of the overlaoding that we are doing,
// it's quite a bit of clearer to just write this out the traditional way.

template<typename T>
class ReadMethodClobberize {
public:
    ReadMethodClobberize(T& value)
        : m_value(value)
    {
    }
    
    void operator()(AbstractHeap heap) const
    {
        m_value.read(heap);
    }
private:
    T& m_value;
};

template<typename T>
class WriteMethodClobberize {
public:
    WriteMethodClobberize(T& value)
        : m_value(value)
    {
    }
    
    void operator()(AbstractHeap heap) const
    {
        m_value.write(heap);
    }
private:
    T& m_value;
};

template<typename T>
class DefMethodClobberize {
public:
    DefMethodClobberize(T& value)
        : m_value(value)
    {
    }
    
    void operator()(PureValue value) const
    {
        m_value.def(value);
    }
    
    void operator()(HeapLocation location, LazyNode node) const
    {
        m_value.def(location, node);
    }

private:
    T& m_value;
};

template<typename Adaptor>
void clobberize(Graph& graph, Node* node, Adaptor& adaptor)
{
    ReadMethodClobberize<Adaptor> read(adaptor);
    WriteMethodClobberize<Adaptor> write(adaptor);
    DefMethodClobberize<Adaptor> def(adaptor);
    clobberize(graph, node, read, write, def);
}

} } // namespace JSC::DFG

#endif // ENABLE(DFG_JIT)
