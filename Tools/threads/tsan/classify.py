#!/usr/bin/env python3
import re, sys, collections

RULES = [  # (family-id, regex over "a ||| b" key string) — first match wins
 ('exec-counter', r'ExecutionCounter|shouldOptimizeNowFromBaseline|osrExitCounter|checkIfOptimizationThresholdReached|operationOptimize|shouldTriggerFTLCompile|setOptimizationThresholdBasedOnCompilationResult|jitCompileAndSetHeuristics|tierUpCommon|countReoptimization|reoptimizationRetryCounter|setDidTryToEnterInLoop|setDidOptimize|didOptimize|UnlinkedMetadataTable::setDidOptimize'),
 ('value-profile', r'ValueProfile|mergeSpeculation|UnlinkedValueProfile|LazyValueProfile|iteratorOpenTryFast|iteratorNextTryFast|IteratorOpen|IteratorNext|ArgumentValueProfile|profile_catch|ConcurrentVector<JSC::LazyOperandValueProfile'),
 ('arith-profile', r'ArithProfile|observeLHSAndRHS|updateArithProfileForBinaryArithOp'),
 ('array-profile', r'ArrayProfile|ArrayProfileFlag'),
 ('array-alloc-profile', r'ArrayAllocationProfile|CompactPointerTuple<JSC::JSArray'),
 ('obj-alloc-profile', r'ObjectAllocationProfile'),
 ('simple-stats', r'SimpleStats'),
 ('metaallocator-stats', r'MetaAllocator'),
 ('linkbuffer-stats', r'LinkBuffer::performFinalization|AbstractMacroAssemblerBase::initializeRandom'),
 ('calllink', r'CallLinkInfo|PolymorphicCallStubRoutine|noticeIncomingCall|linkFor|linkPolymorphicCall|repatchSlowPathCall|CallLinkRecord|DataOnlyCallLinkInfo'),
 ('ic-stubinfo', r'PropertyInlineCache|HandlerPropertyInlineCache|GaveUp|ICSlowPathCallFrameTracer|publishHandlerChainHead|SharedJITStubSet|InlineCacheCompiler|GetByIdMode|emit_op_get_by_id|PutByStatus|emit_op_resolve_scope|resolve_scope|emitSlow_op_resolve_scope|JITCodeMap|repatch'),
 ('watchpoints', r'WatchpointSet|clobberize'),
 ('code-lifecycle', r'installCode|hasJITCodeFor|setJITCode|CodeBlock::jitType|CodeBlock::jitCode|replaceCodeBlockWith|CodeBlock::replacement|codeBlockFor|baselineCodeBlockFor|JITCode::JITCode|JITCode::jitType|JITCode::isUnlinked|DirectJITCode|FTL::JITCode|capabilityLevel|prepareForExecution|hasInstalledVMTrapsBreakpoints|invalidateLinkedCode|prepareOSREntry|operationCompileOSRExit|OSRExit|DFG::compileImpl|ExecutableBase'),
 ('codeblock-init', r'CodeBlock::CodeBlock|setupWithUnlinkedBaselineCode|CodeBlock::setNumParameters|CodeBlock::numParameters|setConstantRegisters|constantRegister|MetadataTable|setBaselineJITData|baselineJITData|finishCreation|CatchInfo|bytecodeCost|CodeBlock::globalObject|CodeBlock::vm|CodeBlock::scopeRegister|CodeBlock::instructions|CodeBlock::ownerExecutable|CodeBlock::isConstructor|CodeBlock::alternative|CodeBlock::unlinkedCodeBlock|couldBeTainted|UnlinkedCodeBlock|InstructionStream|ensureCatchLiveness|livenessAnalysis|BaselineJITCode|fullnessRate|livenessRate|CompressedLazy|forEachValueProfile|UnlinkedFunctionExecutable|ScriptExecutable::recordParse|isInStrictContext|executeCachedCall|prepareForCachedCall|unlinkOrUpgradeImpl|CachedCall'),
 ('interp-exec', r'Interpreter::executeCallImpl|Interpreter::executeProgram|executeCall'),
 ('microtasks', r'MicrotaskQueue'),
 ('tinybloom', r'TinyBloomFilter'),
 ('property-table', r'PropertyTable|addAfterFind'),
 ('structure-fields', r'Structure::Structure|Structure::setMaxOffset|Structure::maxOffset|Structure::previousID|setPreviousID|Structure::hasRareData|Structure::rareData|allocateRareData|propertyTableOrNull|setPropertyTable|ensurePropertyTable|propertyHash|Structure::add|classInfoForCells|Structure::typeInfo|Structure::realm|nonPropertyTransition|prototypeChain|StructureChain|StructureRareData|cachedPropertyNameEnumerator|canCachePropertyNameEnumerator|StructureChainInvalidationWatchpoint|StructureTransitionTable|fireStructureTransitionWatchpoint|JSPropertyNameEnumerator|enumerator_next|setCachedPropertyNameEnumerator|ensurePropertyReplacementWatchpointSet|incrementActiveReplacementWatchpointSet'),
 ('cell-header', r'JSCell::JSCell|JSCell::setStructure|JSCell::clearStructure|JSCell::cellState|setCellState|cellHeaderConcurrentLoad|TypeInfoBlob|setStructureIDDirectly|dcasHeaderAndButterfly|WriteBarrierStructureID|nukeStructureAndSetButterfly'),
 ('jsvalue-slots', r'WriteBarrierBase|updateEncodedJSValueConcurrent|Register::jsValue|Register::operator=|Register::codeBlock|SparseArray|putIndexedDescriptor|tryGetIndexQuicklyConcurrent|trySetIndexQuicklyConcurrent|ContiguousData'),
 ('butterfly-words', r'taggedButterflyWord|AuxiliaryBarrier|IndexingHeader|ensureLengthSlowConcurrent|createInitialIndexedStorageConcurrent|tryGrowSegmentedVectorLength|shrinkButterflyForSetLength|Butterfly::tryCreate|Butterfly::createOrGrow|classifyConcurrentLockedAdd|FreeCell::makeLast'),
 ('typedarray-sab', r'JSArrayBufferView|CagedPtr|ArrayBufferContents|zeroFill|getData|TypedArrayView|typedArrayView|genericTypedArrayView|getIndexQuicklyForTypedArray|trySetIndexQuicklyForTypedArray|canGetIndexQuicklyForTypedArray|ArrayBuffer::transferTo|JSDataView|possiblySharedBufferImpl|exchangeAdd<int>'),
 ('gc-incoming-ref', r'GCIncomingRefCounted|JSArrayBuffer'),
 ('remembered-set', r'addToRememberedSet'),
 ('block-directory-bits', r'BlockDirectoryBits|FastBitReference'),
 ('gc-marking-residual', r'GCSegmentedArray|IsoCellSet|visitCount|appendToMarkStack|BitSet'),
 ('strings-ropes', r'JSRopeString|StringImplShape|StringImpl|WTF::String|JSString|equalCommon|copyElements|unalignedLoad|StringTypeAdapter|resolveToBuffer|StringView|StringRecursionChecker|atomize'),
 ('date-cache', r'DateCache|DateInstance|GregorianDateTime'),
 ('regexp-shared', r'RegExpCachedResult|MatchingContextHolder|RegExp'),
 ('symbol-registry', r'WeakGCMap|Symbol|WeakImpl|Weak<'),
 ('vm-shared-misc', r'VM::updateSoftReservedZoneSize|VM::ensureTerminationException|LazyProperty|LazyClassStructure|ScratchBuffer|SmallStrings|NumericStrings|KeyAtomString|realpath|icu_75|timeZoneCache'),
 ('directarguments', r'DirectArguments|GenericArgumentsImpl|JSCallee|JSFunction|FunctionRareData|create_this|scopeUnchecked'),
 ('heap-misc', r'incrementWithSaturation|VectorBufferBase|VectorBuffer|allocateBuffer|VectorTypeOperations|TrailingArray|FixedVector|RefCountedBase|ThreadSafeRefCounted|MallocPtr|HashTraits|free \|\|\| malloc|JSValue'),
]

def classify(key):
    s = ' ||| '.join(key)
    for fid, rx in RULES:
        if re.search(rx, s):
            return fid
    return 'UNMATCHED'

def main():
    fams = collections.defaultdict(lambda: [0, []])
    cur = None
    for line in open(sys.argv[1]):
        m = re.match(r"FAMILY count=(\d+) key=\((.*)\)\s*$", line)
        if m:
            cnt = int(m.group(1))
            key = tuple(re.findall(r"'((?:[^'\\]|\\.)*)'", m.group(2)))
            fid = classify(key)
            fams[fid][0] += cnt
            fams[fid][1].append((cnt, key))
    total = 0
    for fid, (cnt, keys) in sorted(fams.items(), key=lambda kv: -kv[1][0]):
        total += cnt
        print(f'{fid}: {cnt} reports, {len(keys)} stack-pair keys')
        for c, k in sorted(keys, reverse=True)[:4]:
            print(f'    {c}x {k}')
    print('TOTAL', total)

main()
