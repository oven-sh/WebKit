/*
 * Copyright (C) 2019-2026 Apple Inc. All rights reserved.
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

#include <JavaScriptCore/GCLogging.h>
#include <JavaScriptCore/JSCWebPreferenceOptions.h>
#include <JavaScriptCore/JSExportMacros.h>
#include <JavaScriptCore/OSCheck.h>
#include <wtf/MathExtras.h>

#if OS(DARWIN)
#include <mach/vm_param.h>
#endif

using WTF::PrintStream;

namespace JSC {

#define MAXIMUM_NUMBER_OF_FTL_COMPILER_THREADS 8

JS_EXPORT_PRIVATE bool canUseJITCage();
bool canUseWasm();
bool hasCapacityToUseLargeGigacage();

// How do JSC VM options work?
// ===========================
// The FOR_EACH_JSC_OPTION() macro below defines a list of all JSC options in use,
// along with their types and default values. The options values are actually
// realized as fields in OptionsStorage embedded in JSC::Config.
//
//     Options::initialize() will initialize the option values with the defaults
// specified in FOR_EACH_JSC_OPTION() below. After that, the values can be
// programmatically read and written to using an accessor method with the same
// name as the option. For example, the option "useJIT" can be read and set like
// so:
//
//     bool jitIsOn = Options::useJIT();  // Get the option value.
//     Options::useJIT() = false;         // Sets the option value.
//
//     If you want to tweak any of these values programmatically for testing
// purposes, you can do so in Options::initialize() after the default values
// are set.
//
//     Alternatively, you can override the default values by specifying
// environment variables of the form: JSC_<name of JSC option>.
//
// Note: Options::initialize() tries to ensure some sanity on the option values
// which are set by doing some range checks, and value corrections. These
// checks are done after the option values are set. If you alter the option
// values after the sanity checks (for your own testing), then you're liable to
// ensure that the new values set are sane and reasonable for your own run.
//
// Any modifications to options must be done before the first VM is instantiated.
// On instantiation of the first VM instance, the Options will be write protected
// and cannot be modified thereafter.

#if USE(BUN_JSC_ADDITIONS)
#define FOR_EACH_JSC_FFI_OPTION(v) \
    v(Bool, useFFIICStub, true, Normal, "install per-function FFI IC stubs"_s) \
    v(Bool, useFFICallInDFG, true, Normal, "allow Call -> CallFFI in DFG/FTL"_s) \
    v(Bool, useFFIDirectCall, true, Normal, "FTL calls the native FFI target directly (no invoke thunk)"_s) \
    v(Bool, dumpFFIDisassembly, false, Normal, "disassemble generated FFI thunks/stubs"_s) \
    v(Bool, verboseFFI, false, Normal, "dataLog on FFI thunk/stub/signature creation"_s)
#define FOR_EACH_JSC_CODEBLOCK_AGING_OPTION(v) \
    v(Bool, useExecutionCountForCodeBlockAging, true, Normal, "If true, an LLInt/Baseline CodeBlock whose execution counter has advanced since the last old-age check is treated as still in use and its TTL is renewed instead of being jettisoned."_s) \
    v(Unsigned, optimizedCodeAgingQuietAllocationMB, 1, Normal, "A collection that finds more than this much allocated since the last one that did marks the mutator as active for the aging of FTL code (and DFG code without a tier-up counter); an embedder-tagged idle collection lets such code go once nothing has been active for optimizedCodeAgingQuietSeconds. 0 = such code never ages out."_s) \
    v(Double, optimizedCodeAgingQuietSeconds, 30, Normal, "How long since the last active collection (and since the code was installed) before an idle collection lets such code go (capped at the tier's TTL under useEagerCodeBlockJettisonTiming)."_s) \
    v(Double, codeBlockAgingLeaseMultiplier, 3.0, Normal, "When useExecutionCountForCodeBlockAging proves a CodeBlock is still active, renew its old-age TTL to this many multiples of timeToLive for its tier."_s) \
    v(Bool, useSharedModuleFunctionExpressionExecutables, false, Normal, "If true, the FunctionExecutables of the function expressions and classes in a module's top-level code belong to its ModuleProgramExecutable rather than to each linked ModuleProgramCodeBlock, so their CodeBlocks and JIT code outlive the module's own linked code and are shared by every evaluation of the module."_s) \
    v(Bool, useRunOnceCodeRelease, true, Normal, "If true, a module program lets go of its CodeBlock, unlinked code block (and its CodeCache entry) as soon as its evaluation has finished (unless records of several module loaders share it, which run the same code again), and a program does once it has run, instead of keeping them for as long as any function they created lives."_s)
#define FOR_EACH_JSC_BYTECODE_CACHE_DECODER_OPTION(v) \
    v(Bool, diskCachePayloadIsPersistentForTesting, false, Normal, "jsc shell: keep files mapped from diskCachePath for the life of the process and mark them persistent, so what is decoded from them borrows from and defers into the mapping as it does with an embedded payload."_s) \
    v(Bool, usePrelinkedModuleInfo, true, Normal, "If true, module records the embedder creates from a pre-resolved module graph (PrelinkedModuleGraph) keep their entries in the graph: requested modules are wired by index, import/export resolution, GetImportedModule, InitializeEnvironment and GetModuleNamespace read the graph's tables, and the by-name entry maps are only built on demand. If false such records copy their entries out of the graph and behave like ModuleAnalyzer's."_s) \
    v(Bool, validatePrelinkedModuleInfo, false, Normal, "Cross-check every pre-resolved import/export binding of a PrelinkedModuleGraph against the specification's ResolveExport and crash on a mismatch."_s)
#else
#define FOR_EACH_JSC_FFI_OPTION(v)
#define FOR_EACH_JSC_CODEBLOCK_AGING_OPTION(v)
#define FOR_EACH_JSC_BYTECODE_CACHE_DECODER_OPTION(v)
#endif

#define FOR_EACH_JSC_OPTION(v)                                          \
    v(Bool, useKernTCSM, defaultTCSMValue(), Normal, "Note: this needs to go before other options since they depend on this value."_s) \
    v(Bool, validateOptions, false, Normal, "crashes if mis-typed JSC options were passed to the VM"_s) \
    v(Unsigned, dumpOptions, 0, Normal, "dumps JSC options (0 = None, 1 = Overridden only, 2 = All, 3 = Verbose)"_s) \
    v(OptionString, configFile, nullptr, Normal, "file to configure JSC options and logging location"_s) \
    \
    v(Bool, useLLInt,  true, Normal, "allows the LLINT to be used if true"_s) \
    v(Bool, useJIT, jitEnabledByDefault(), Normal, "allows the executable pages to be allocated for JIT and thunks if true"_s) \
    v(Bool, useBaselineJIT, true, Normal, "allows the baseline JIT to be used if true"_s) \
    v(Bool, useDFGJIT, jitEnabledByDefault(), Normal, "allows the DFG JIT to be used if true"_s) \
    v(Bool, useRegExpJIT, jitEnabledByDefault(), Normal, "allows the RegExp JIT to be used if true"_s) \
    v(Bool, useDOMJIT, jitEnabledByDefault(), Normal, "allows the DOMJIT to be used if true"_s) \
    v(Bool, useRegExpLookbehindJIT, true, Normal, "allows patterns containing lookbehind assertions to use the RegExp JIT"_s) \
    v(Bool, useRegExpAlternationFactoring, true, Normal, "factors shared prefixes out of wide alternations and folds wide top-level alternations into a group"_s) \
    v(Bool, useRegExpAlternationDispatch, true, Normal, "lets the RegExp JIT dispatch a group's alternatives on their first character and compare short literal alternatives inline"_s) \
    v(Unsigned, regExpDispatchMaxInlineLiteralLength, 32, Normal, "longest literal alternative (up to the JIT's ceiling of 32) the RegExp JIT compares inline inside a first-character dispatch chain; 0 disables inline literals"_s) \
    v(Bool, useLazyRegExpPatternConstruction, true, Normal, "RegExp creation only syntax-checks and capture-counts patterns longer than 64 characters (other than pure literals, named-group and deeply nested patterns) instead of building a YarrPattern it then discards; the pattern is first built when the RegExp is compiled, so YarrPatternConstructor::setupOffsets errors surface there and its error code can differ from the eager one"_s) \
    \
    v(Bool, reportMustSucceedExecutableAllocations, false, Normal, nullptr) \
    /* Bun Features */\
    v(Bool, useV8DateParser, false, Normal, nullptr) \
    v(Bool, showPrivateScriptsInStackTraces, false, Normal, "Show private scripts in stack traces."_s) \
    v(Bool, evalMode, false, Normal, "Set to true for less aggressive function call completion value discarding."_s) \
    FOR_EACH_JSC_FFI_OPTION(v) \
    \
    v(Unsigned, maxPerThreadStackUsage, 5 * MB, Normal, "Max allowed stack usage by the VM"_s) \
    v(Unsigned, softReservedZoneSize, 128 * KB, Normal, "A buffer greater than reservedZoneSize that reserves space for stringifying exceptions."_s) \
    v(Unsigned, reservedZoneSize, 64 * KB, Normal, "The amount of stack space we guarantee to our clients (and to interal VM code that does not call out to clients)."_s) \
    \
    v(Bool, crashOnDisallowedVMEntry, ASSERT_ENABLED, Normal, "Forces a crash if we attempt to enter the VM when disallowed"_s) \
    v(Bool, crashIfCantAllocateJITMemory, false, Normal, nullptr) \
    v(Unsigned, structureHeapSizeInKB, 0, Normal, "Override for Structure Heap size (in KBs) if non-zero"_s) \
    v(Unsigned, jitMemoryReservationSize, 0, Normal, "Set this number to change the executable allocation size in ExecutableAllocatorFixedVMPool. (In bytes.)"_s) \
    v(Size, jitMemoryReservationAddress, 0, Restricted, "If non-zero, we will attempt to allocate JIT memory at the address provided and crash if we cannot.") \
    \
    v(Bool, forceCodeBlockLiveness, false, Normal, nullptr) \
    v(Bool, forceICFailure, false, Normal, nullptr) \
    v(Bool, forceUnlinkedDFG, false, Normal, nullptr) \
    \
    v(Bool, aotVerbose, false, Normal, "Log what the static compiler compiles and why it declines a function."_s) \
    v(Bool, aotDumpGraph, false, Normal, "Dump the static compiler's IR."_s) \
    v(Bool, aotDumpB3, false, Normal, "Dump the B3 the static compiler produces."_s) \
    v(Bool, aotDumpDisassembly, false, Normal, "Dump the machine code the static compiler produces."_s) \
    v(Bool, aotReportStats, false, Normal, "At exit, report how many functions the static compiler compiled and the reasons it declined the others."_s) \
    v(Bool, aotForceVeneers, false, Normal, "In an image, make every direct call from one function to another the way one that is out of reach is made. For testing."_s) \
    v(OptionString, aotFilter, nullptr, Normal, "Only functions whose name contains this string are compiled by the static compiler."_s) \
    v(Unsigned, aotB3OptLevel, 2, Normal, "B3 optimization level for the static compiler."_s) \
    v(OptionString, aotImagePath, nullptr, Normal, "An image of code from the static compiler: functions it has code for run that code."_s) \
    v(Unsigned, aotReportSlowPaths, 0, Normal, "If not zero: every that many times an operation of the static compiler's that keeps count is called, say which were called most, and for what."_s) \
    v(Bool, definePlainInstanceFieldsInConstructor, false, Normal, "The constructor of a class all of whose instance fields are a name and nothing else (class C { a; b; }) defines them itself, instead of calling a function that does."_s) \
    v(Bool, evaluateObjectLiteralValuesFirst, false, Normal, "The values of the properties of an object literal are all worked out before the object is made, where nobody can tell: making the object and giving it its properties is then one run of instructions."_s) \
    v(Bool, resolveAllScopeSlotsStatically, false, Normal, "The bytecode optimizer gives get_from_scope the slot of a variable it can prove the place of even where that makes the instruction wider."_s) \
    v(Unsigned, aotThreads, 0, Normal, "How many threads compile an image. Zero: as many as there are processors."_s) \
    v(Unsigned, aotLimit, 0, Normal, "If not zero, the static compiler only compiles this many functions."_s) \
    v(Unsigned, aotDisableFastPaths, 0, Normal, "For debugging the static compiler. 1: get_by_id cache, 2: put_by_id replace, 4: put_by_id transition, 8: get_by_val, 16: put_by_val, 32: scope caches, 64: closure variable stores, 128: tail calls (made as calls), 256: get_by_id on the prototype chain, 512: put_by_id transitions that are not direct, 1024: the megamorphic cache, 2048: allocation without the runtime, 4096: the equality thunks."_s) \
    v(Bool, aotSplitLoops, true, Normal, "The static compiler makes two copies of every loop: one that only does what is quick, and leaves for the other when it cannot."_s) \
    v(Bool, staticHeapGuardsShortFunctionExecutables, false, Normal, "For testing. Each FunctionExecutable in the short form is the last thing on a page, and there is nothing at the addresses of the page after it: whatever takes one for more than it is crashes."_s) \
    v(Bool, staticHeapMakesShortFunctionExecutables, true, Normal, "In a static heap that goes without bytecode, the FunctionExecutable of a function that there is nothing out of the ordinary to say about is in the short form."_s) \
    v(Bool, aotCallsBoundFunctionsWithStub, true, Normal, "Where there is no JIT to make a thunk for it, a bound function whose target is a function is called by a stub that makes the frame of the target, and not by way of C++."_s) \
    v(Bool, useMarkedPrototypesForMegamorphicCache, true, Normal, "A change to an object that may be a prototype only invalidates the megamorphic cache if something in the cache goes by that object."_s) \
    v(Bool, aotLooksInMegamorphicCacheUnlessSlotIsEmpty, true, Normal, "A site whose slot failed it looks in the megamorphic cache before it goes to C++, unless the slot is its own and has nothing in it yet. If not: only once it has given up on the slot. (When the stubs are made.)"_s) \
    v(Bool, aotCachesConstructionForManyFunctions, true, Normal, "Where a constructor makes its object and stores to it, and the site knows another function than the one that is being constructed with, what the object ends up as is looked for in a table all such sites share."_s) \
    v(Bool, aotTypesParametersOfClosedFunctions, true, Normal, "A function that only the calls of it that the compiler can see get to call is compiled for what those pass it."_s) \
    v(Bool, aotPassesValuesUnboxed, true, Normal, "With aotTypesParametersOfClosedFunctions: a closed function is passed what is always a number or a boolean as that, and hands such a thing back as that."_s) \
    v(Bool, aotCompilesBuiltins, true, Normal, "The image has code for the engine's own functions that are written in JavaScript."_s) \
    v(Unsigned, additionsBeforeLoneObjectIsDictionary, 0, Normal, "An object that has been given that many properties one after the other, none of which any other object was given after the same ones, becomes a dictionary: it is not given a Structure for each of the rest. Zero: never."_s) \
    v(Bool, aotCountsAllocations, false, Normal, "TEMPORARY. When compiling: code counts what it makes, by what the compiler made of what becomes of it (Escape)."_s) \
    v(OptionString, aotTypeTable, nullptr, Normal, "When compiling: the types of the program (AOT::TypeTable), which its text refers to by number (useTypeTags)."_s) \
    v(Unsigned, aotShapes, 15, Normal, "What is made of the shapes in the table of types. 1: literals are laid out as it says. 2: reads go by it. 4: so do writes. 8: what a layout is known not to have is undefined."_s) \
    v(Bool, aotInlines, true, Normal, "A call of a function that is proven to be the callee is replaced by what the function does, if that is little or if it is the only call there is."_s) \
    v(Bool, aotInlinesBuiltins, true, Normal, "aotInlines, aotCompilesBuiltins, useImmutableIntrinsics: likewise a call of a method by the name of one of Array.prototype's that is passed a closure made on the spot, after a check that it is that one."_s) \
    v(Unsigned, aotInlinesUpTo, 60, Normal, "aotInlines: how many bytes of bytecode a function may have that is called from several places."_s) \
    v(Unsigned, aotInlinesOnlyCallUpTo, 1200, Normal, "aotInlines: how many if it is called from one place, and by nothing else."_s) \
    v(Unsigned, aotInlinesAtMost, 4000, Normal, "aotInlines: how many bytes of bytecode one function takes over in all."_s) \
    v(Bool, aotVerifiesFacts, false, Normal, "For testing the compiler: wherever a value is taken to be of some type without being looked at, it is looked at, and if it is not of that type the process ends."_s) \
    v(Bool, aotTypesVariables, true, Normal, "What is read from a variable that lives in an environment record, of a module or of a function, is taken to be one of the things that the program puts there."_s) \
    v(Bool, aotKnowsWhatBuiltinsReturn, true, Normal, "What comes back from a function of the language itself, where the program cannot have put another in its place, is taken to be what the specification says it is."_s) \
    v(Bool, aotLogsFacts, false, Normal, "For finding out why the compiler took something for a fact: once the whole of the program has been looked at, says what it found of each closed function and of each variable, and what it went by. Lines that start with FACTLOG."_s) \
    v(Bool, aotTrustsDeclaredTypes, false, Normal, "UNSOUND, for measuring what checks cost and nothing else: there is no code for op_check_type, and what it is given is taken to be what it would have let by. With aotVerifiesFacts, says where that is not so."_s) \
    v(Bool, aotResolvesScopesItself, true, Normal, "Where a variable of the code around a function is goes by what that code declares, whether or not the bytecode has been rewritten to say so."_s) \
    v(Unsigned, aotFacts, 0, Normal, "EXPERIMENT, for sizing; what comes of it does not run. $$t(value, n) with n >= 1 << 28 is no check: n is something a type checker would have told the compiler. A bit for each thing done with them: 1 fields at fixed offsets, 2 what fields and elements hold, 4 direct calls, 8 builtins without a lookup, 16 for-of over arrays, 32 elements, 64 nothing (they are taken out, that is all)."_s) \
    v(Bool, aotStoresHomedRegistersOnlyWhereRead, true, Normal, "A register that a handler reads is written to memory only where such a handler can be got to before it is written again, rather than everywhere in the function."_s) \
    v(Bool, aotCallIntrinsics, true, Normal, "A call of what may be one of the functions that the static compiler's stubs know a quick way with goes by way of a stub that finds out (AOT::StubIntrinsic)."_s) \
    v(Unsigned, aotCallIntrinsicsMustBeRight, 0, Normal, "For testing. A bit for each StubIntrinsic: a call that its stub cannot see to itself is a trap."_s) \
    v(Unsigned, aotLoopsToSplit, 3, Normal, "Which loops aotSplitLoops is for. 1: all. 2: those with something in them that the fast copy does better (arithmetic, elements got at by index, a call that it does not make). 3: those in which no call is made. 4: both. 5: those in which no call is made, or that have a call that the fast copy does not make or an element got at by its index."_s) \
    v(Unsigned, aotSkip, 0, Normal, "If not zero, the static compiler leaves out the function that would be this many-th."_s) \
    \
    v(Unsigned, repatchCountForCoolDown, 8, Normal, nullptr) \
    v(Unsigned, initialCoolDownCount, 20, Normal, nullptr) \
    v(Unsigned, repatchBufferingCountdown, 6, Normal, nullptr) \
    v(Unsigned, initialRepatchBufferingCountdown, 6, Normal, nullptr) \
    \
    v(Bool, dumpGeneratedBytecodes, false, Normal, nullptr) \
    v(Bool, useUnboxedFastArrayIteration, true, Normal, "for-of and array destructuring over an Array keep the index in the frame instead of allocating an Array Iterator object"_s) \
    v(Bool, useSharedRegExpLiteralObjects, true, Normal, "reuse one RegExpObject per literal site when the object provably cannot be observed"_s) \
    v(Bool, aotStartFunctionsCold, true, Normal, "A function from an image that can do without gets nothing of its own until it has been run a few times: see AOT::SharedData."_s) \
    v(Unsigned, aotMissesForEightSlots, 8, Normal, "How often a slot may fail a function that started with none of its own before it gets them: this many times for every eight slots it would have, ..."_s) \
    v(Unsigned, aotMissesToSpare, 4, Normal, "... and this many more."_s) \
    v(Bool, aotKeepsQuotes, false, Normal, "An image says what the source says wherever an error message may quote it, for a program that is built without its text."_s) \
    v(Bool, useStaticHeapInEveryVM, true, Normal, "VMs other than the first of a process refer to what is in its static heap too, and run the program's code from its image the way the first does."_s) \
    v(Unsigned, staticHeapModuleToRefuseOtherVMs, 0, Normal, "For testing: VMs other than the first load this module of the static heap (counting from one) as if it were not there."_s) \
    v(Bool, staticHeapHasBuiltinFunctions, true, Normal, "When a program is built, the embedder's builtin functions that are linked with it are made ahead of time as its modules are."_s) \
    v(Bool, staticHeapLeavesOutPayload, false, Normal, "When a program is built, its static heap has none of the bytecode it was built from, only what says where in the source each instruction came from."_s) \
    v(Bool, aotNumbersIdentifiersOfProgram, true, Normal, "The code of a program that goes without its bytecode says which name it means by a number that is the same in all of its functions."_s) \
    v(Bool, aotNumbersConstantsOfProgram, true, Normal, "And which constant, likewise."_s) \
    v(Bool, aotCompileRegExps, true, Normal, "An image has code for the regular expressions that the program's code has in it. When the program runs: they use it."_s) \
    v(Bool, aotCompileStringsThatLookLikeRegExps, false, Normal, "And for the strings in it that look as if regular expressions are made of them."_s) \
    v(Bool, staticHeapForgetsNamesOfVariables, true, Normal, "When a program is built, the SymbolTables of its static heap keep only the names that its code may look up when it runs."_s) \
    v(Bool, staticHeapKeepsFunctionCode, false, Normal, "When a static heap is built: the unlinked code of functions that were compiled is decoded into it, rather than left in the payload for whoever asks."_s) \
    v(Bool, useImmutableIntrinsics, false, Normal, "What Object.prototype, Array.prototype, Math and the like have when a realm is made stays as it is: see JSGlobalObject::makeIntrinsicsImmutable()."_s) \
    v(Bool, useTypeTags, false, Normal, "The text of a program may say something of a token that is no part of the language: a byte that is 1, and six characters that are a number (Lexer::readTypeTag()). It is what whoever made the text knows of the types there."_s) \
    v(Bool, useSoundTypes, false, Normal, "compile $$t(value, <integer literal mask>) calls to op_check_type instead of a call"_s) \
    v(Bool, iterateCheckedArraysByIndex, true, Normal, "With useSoundTypes and useImmutableIntrinsics: for (x of $$t(e, array)) goes through the array by index, having seen to it that iterating over it is what it is for any array."_s) \
    v(Bool, reportSoundTypeViolations, false, Normal, "A type check that fails says so, once for each place, instead of throwing. Not for code from the static compiler, which relies on the checks."_s) \
    v(Bool, ignoreArgumentProfilesForTesting, false, Normal, "the DFG treats the value profiles of arguments as empty, as if the function had never been called"_s) \
    v(Bool, useBytecodeOptimizer, false, Normal, "run the whole-function bytecode optimizer on all generated bytecode (bytecode-cache image generation runs it when the embedder passes OptimizeBytecode::Yes)") \
    v(Bool, useBytecodeOptimizerCopyPropagation, true, Normal, "bytecode optimizer: copy propagation / destination coalescing") \
    v(Bool, useBytecodeOptimizerTDZ, true, Normal, "bytecode optimizer: redundant TDZ check elimination") \
    v(Bool, useBytecodeOptimizerScopeCache, true, Normal, "bytecode optimizer: cache environment-record scope resolutions in fresh registers") \
    v(Bool, useBytecodeOptimizerStaticScopes, true, Normal, "bytecode optimizer: resolve environment-record variables statically (no abstractResolve at link)") \
    v(Bool, validateBytecodeOptimizerStaticScopes, false, Normal, "check statically resolved scopes against JSScope::abstractResolve when linking") \
    v(Bool, dumpBytecodeOptimizer, false, Normal, "dump the bytecode optimizer IR") \
    v(Bool, reportBytecodeOptimizer, false, Normal, "report per-code-block bytecode optimizer statistics") \
    v(Bool, dumpBytecodeLivenessResults, false, Normal, nullptr) \
    v(Bool, validateBytecode, false, Normal, nullptr) \
    v(Bool, forceDebuggerBytecodeGeneration, false, Normal, nullptr) \
    v(Bool, debuggerTriggersBreakpointException, false, Normal, "Using the debugger statement will trigger an breakpoint exception (Useful when lldbing)"_s) \
    v(Bool, verboseWasmDebugger, false, Normal, nullptr) \
    v(Bool, enableWasmDebugger, false, Normal, nullptr) \
    v(Bool, verboseWasmTypeCleanup, false, Normal, "Log per-invocation counts from Wasm::TypeInformation::tryCleanup (scanned / live / reclaimed)."_s) \
    v(Bool, dumpBytecodesBeforeGeneratorification, false, Normal, nullptr) \
    v(Unsigned, switchJumpTableAmountThreshold, 15, Normal, nullptr) \
    \
    v(Bool, useFunctionDotArguments, true, Normal, nullptr) \
    v(Bool, useTailCalls, true, Normal, nullptr) \
    v(Bool, optimizeRecursiveTailCalls, true, Normal, nullptr) \
    v(Bool, alwaysUseShadowChicken, false, Normal, nullptr) \
    v(Unsigned, shadowChickenLogSize, 1000, Normal, nullptr) \
    v(Unsigned, shadowChickenMaxTailDeletedFramesSize, 128, Normal, nullptr) \
    \
    v(OSLogType, useOSLog, OSLogType::None, Normal, "Log dataLog()s to os_log instead of stderr"_s) \
    /* dumpDisassembly implies dumpDFGDisassembly. */ \
    v(Bool, needDisassemblySupport, false, Normal, nullptr) \
    v(Bool, dumpDisassembly, false, Normal, "dumps disassembly of all JIT compiled code upon compilation"_s) \
    v(Bool, logJIT, false, Normal, nullptr) \
    v(Bool, dumpBaselineDisassembly, false, Normal, "dumps disassembly of Baseline function upon compilation"_s) \
    v(Bool, dumpDFGDisassembly, false, Normal, "dumps disassembly of DFG function upon compilation"_s) \
    v(Bool, dumpFTLDisassembly, false, Normal, "dumps disassembly of FTL function upon compilation"_s) \
    v(Bool, dumpCSSJITDisassembly, false, Normal, "dumps disassembly of CSS Selector JIT upon compilation"_s) \
    v(Bool, dumpRegExpDisassembly, false, Normal, "dumps disassembly of RegExp upon compilation"_s) \
    v(Bool, traceRegExpJITExecution, false, Normal, "traces RegExp JIT execution at reentry points"_s) \
    v(Bool, verifyRegExpJITReads, false, Normal, "checks, before every load the RegExp JIT makes from the subject string, that the address lies within the subject (crashes otherwise); a fuzzing aid"_s) \
    v(Bool, dumpWasmDisassembly, false, Normal, "dumps disassembly of all wasm code upon compilation"_s) \
    v(OptionString, dumpWasmSourceFileName, nullptr, Normal, "log every wasm module validation, and dump source bytes to <filename>.0.wasm, <filename>.1.wasm, etc..."_s) \
    v(OptionString, wasmOMGFunctionsToDump, nullptr, Normal, "file with newline separated list of function indices to dump IR/disassembly for, if no such file exists, the function index itself"_s) \
    v(Bool, dumpBBQDisassembly, false, Normal, "dumps disassembly of BBQ wasm code upon compilation"_s) \
    v(Bool, dumpOMGDisassembly, false, Normal, "dumps disassembly of OMG wasm code upon compilation"_s) \
    v(Bool, useJITDump, false, Normal, "generates JITDump side-data") \
    v(Bool, useGdbJITInfo, false, Normal, "generates GDB JIT API side-data; to use with lldb on macos, add `settings set plugin.jit-loader.gdb.enable on` to .lldbinit") \
    v(Bool, useTextMarkers, false, Normal, "generates text markers side-data") \
    v(OptionString, jitDumpDirectory, nullptr, Normal, "Directory to place JITDump"_s) \
    v(Bool, useIRDump, false, Normal, "generates IR dump files and JIT_CODE_DEBUG_INFO in JITDump"_s) \
    v(OptionString, irDumpDirectory, nullptr, Normal, "Directory to place IR dump files"_s) \
    v(Bool, useSourceCodeDump, false, Normal, "generates source code debug info in JITDump"_s) \
    v(OptionString, sourceCodeDumpDirectory, nullptr, Normal, "Directory to place dumped source files"_s) \
    v(OptionString, textMarkersDirectory, nullptr, Normal, "Directory to place MarkerTxt") \
    v(OptionRange, bytecodeRangeToJITCompile, nullptr, Normal, "bytecode size range to allow compilation on, e.g. 1:100"_s) \
    v(OptionRange, bytecodeRangeToDFGCompile, nullptr, Normal, "bytecode size range to allow DFG compilation on, e.g. 1:100"_s) \
    v(OptionRange, bytecodeRangeToFTLCompile, nullptr, Normal, "bytecode size range to allow FTL compilation on, e.g. 1:100"_s) \
    v(OptionString, jitAllowlist, nullptr, Normal, "file with newline separated list of function signatures to allow compilation on or, if no such file exists, the function signature to allow"_s) \
    v(OptionString, dfgAllowlist, nullptr, Normal, "file with newline separated list of function signatures to allow DFG compilation on or, if no such file exists, the function signature to allow"_s) \
    v(OptionString, ftlAllowlist, nullptr, Normal, "file with newline separated list of function signatures to allow FTL compilation on or, if no such file exists, the function signature to allow"_s) \
    v(OptionString, bbqAllowlist, nullptr, Normal, "file with newline separated list of function indices to allow BBQ compilation on or, if no such file exists, the function index to allow"_s) \
    v(OptionString, omgAllowlist, nullptr, Normal, "file with newline separated list of function indices to allow OMG compilation on or, if no such file exists, the function index to allow"_s) \
    v(OptionString, loopUnrollingAllowlist, nullptr, Normal, "file with newline separated list of function signatures to allow loop unrolling on or, if no such file exists, the function signature to allow"_s) \
    v(OptionString, dumpGraphAllowlist, nullptr, Normal, "file with newline separated list of function signatures to filter graph dumps without restricting JIT compilation, or if no such file exists, the function signature to allow (affects dumpGraphAtEachPhase, dumpDFGGraphAtEachPhase, and dumpDFGFTLGraphAtEachPhase)"_s) \
    v(Bool, dumpSourceAtDFGTime, false, Normal, "dumps source code of JS function being DFG compiled"_s) \
    v(Bool, dumpBytecodeAtDFGTime, false, Normal, "dumps bytecode of JS function being DFG compiled"_s) \
    v(Bool, dumpGraphAfterParsing, false, Normal, nullptr) \
    v(Bool, dumpGraphAtEachPhase, false, Normal, nullptr) \
    v(Bool, dumpDFGGraphAtEachPhase, false, Normal, "dumps the DFG graph at each phase of DFG compilation (note this excludes DFG graphs during FTL compilation)"_s) \
    v(Bool, dumpDFGFTLGraphAtEachPhase, false, Normal, "dumps the DFG graph at each phase of DFG compilation when compiling FTL code"_s) \
    v(Bool, dumpB3GraphAtEachPhase, false, Normal, "dumps the B3 graph at each phase of compilation"_s) \
    v(Bool, dumpAirGraphAtEachPhase, false, Normal, "dumps the Air graph at each phase of compilation"_s) \
    v(Bool, verboseDFGBytecodeParsing, false, Normal, nullptr) \
    v(Bool, safepointBeforeEachPhase, true, Normal, nullptr) \
    v(Bool, verboseCompilation, false, Normal, nullptr) \
    v(Bool, verboseFTLCompilation, false, Normal, nullptr) \
    v(Bool, logCompilationChanges, false, Normal, nullptr) \
    v(Bool, printEachOSRExit, false, Normal, nullptr) \
    v(Bool, printEachDFGFTLInlineCall, false, Normal, nullptr) \
    v(Bool, useJITAsserts, ASSERT_ENABLED, Normal, nullptr) \
    v(Bool, validateDoesGC, ASSERT_ENABLED, Normal, nullptr) \
    v(Bool, validateGraph, false, Normal, nullptr) \
    v(Bool, validateGraphAtEachPhase, false, Normal, nullptr) \
    v(Bool, verboseValidationFailure, false, Normal, nullptr) \
    v(Bool, verboseOSR, false, Normal, nullptr) \
    v(Bool, verboseDFGOSRExit, false, Normal, nullptr) \
    v(Bool, verboseFTLOSRExit, false, Normal, nullptr) \
    v(Bool, verboseCallLink, false, Normal, nullptr) \
    v(Bool, verboseCompilationQueue, false, Normal, nullptr) \
    v(Bool, reportCompileTimes, false, Normal, "dumps JS function signature and the time it took to compile in all tiers"_s) \
    v(Bool, reportBaselineCompileTimes, false, Normal, "dumps JS function signature and the time it took to BaselineJIT compile"_s) \
    v(Bool, reportDFGCompileTimes, false, Normal, "dumps JS function signature and the time it took to DFG and FTL compile"_s) \
    v(Bool, reportFTLCompileTimes, false, Normal, "dumps JS function signature and the time it took to FTL compile"_s) \
    v(Bool, reportTotalCompileTimes, false, Normal, nullptr) \
    v(Bool, reportTotalPhaseTimes, false, Normal, "This prints phase times at the end of running script inside jsc.cpp"_s) \
    v(Bool, reportParseTimes, false, Normal, "dumps JS function signature and the time it took to parse"_s) \
    v(Bool, reportBytecodeCompileTimes, false, Normal, "dumps JS function signature and the time it took to bytecode compile"_s) \
    v(Bool, reportBytecodeCacheDecodeTimes, false, Normal, "dumps the time it took to decode bytecode from the disk cache"_s) \
    v(Bool, countParseTimes, false, Normal, "counts parse times"_s) \
    v(Bool, verboseExitProfile, false, Normal, nullptr) \
    v(Bool, verboseCFA, false, Normal, nullptr) \
    v(Bool, verboseDFGFailure, false, Normal, nullptr) \
    v(Bool, verboseFTLToJSThunk, false, Normal, nullptr) \
    v(Bool, verboseFTLFailure, false, Normal, nullptr) \
    v(Bool, testTheFTL, false, Normal, nullptr) \
    v(Bool, verboseSanitizeStack, false, Normal, nullptr) \
    v(Bool, useGenerationalGC, true, Normal, nullptr) \
    v(Bool, useConcurrentGC, true, Normal, nullptr) \
    v(Bool, collectContinuously, false, Normal, nullptr) \
    v(Double, collectContinuouslyPeriodMS, 1, Normal, nullptr) \
    v(Bool, forceFencedBarrier, false, Normal, nullptr) \
    v(Bool, verboseVisitRace, false, Normal, nullptr) \
    v(Bool, optimizeParallelSlotVisitorsForStoppedMutator, false, Normal, nullptr) \
    v(Bool, verboseHeapSnapshotLogging, true, Normal, nullptr) \
    v(Unsigned, largeHeapSize, 32 * 1024 * 1024, Normal, nullptr) \
    v(Unsigned, mediumHeapSize, 4 * 1024 * 1024, Normal, nullptr) \
    v(Unsigned, smallHeapSize, 1 * 1024 * 1024, Normal, nullptr) \
    v(Double, smallHeapRAMFraction, 0.25, Normal, nullptr) \
    v(Double, smallHeapGrowthFactor, 2, Normal, nullptr) \
    v(Double, mediumHeapRAMFraction, 0.5, Normal, nullptr) \
    v(Double, mediumHeapGrowthFactor, 1.5, Normal, nullptr) \
    v(Double, largeHeapGrowthFactor, 1.24, Normal, nullptr) \
    v(Double, miniVMHeapGrowthFactor, 1.20, Normal, nullptr) \
    v(Double, heapGrowthSteepnessFactor, 2.00, Normal, nullptr) \
    v(Double, heapGrowthMaxIncrease, 3.00, Normal, nullptr) \
    v(Double, minEdenToOldGenerationRatio, 1.0 / 3.0, Normal, "after an eden GC, schedule a full collection if remainingHeapSize / maxHeapSize falls below this; bounds the usable heap growth factor below at 1 / (1 - value)"_s) \
    v(Unsigned, heapGrowthFunctionThresholdInMB, 16 * 1024, Normal, nullptr) \
    v(Double, criticalGCMemoryThreshold, 0.80, Normal, "percent memory in use the GC considers critical.  The collector is much more aggressive above this threshold"_s) \
    v(Double, minimumMutatorUtilization, 0, Normal, nullptr) \
    v(Double, maximumMutatorUtilization, 0.7, Normal, nullptr) \
    v(Double, epsilonMutatorUtilization, 0.01, Normal, nullptr) \
    v(Double, concurrentGCMaxHeadroom, 1.5, Normal, nullptr) \
    v(Double, concurrentGCPeriodMS, 2, Normal, nullptr) \
    v(Bool, useStochasticMutatorScheduler, true, Normal, nullptr) \
    v(Double, minimumGCPauseMS, 0.3, Normal, nullptr) \
    v(Double, gcPauseScale, 0.3, Normal, nullptr) \
    v(Double, gcIncrementBytes, 10000, Normal, nullptr) \
    v(Double, gcIncrementMaxBytes, 100000, Normal, nullptr) \
    v(Double, gcIncrementScale, 0, Normal, nullptr) \
    v(Bool, useWarmUpMarkedBlocks, true, Normal, "hand MarkedBlock allocation pages that a helper thread already made resident"_s) \
    v(Unsigned, warmUpMarkedBlockCount, 32, Normal, "how many MarkedBlocks the helper thread keeps ready with their pages already resident; 0 turns it off"_s) \
    v(Unsigned, warmUpMarkedBlockStartAfterBlocks, 64, Normal, "how many MarkedBlocks the process allocates before the helper thread starts; a program that stops before that never creates it"_s) \
    v(Double, warmUpMarkedBlockIdleTimeout, 10, Normal, "seconds without a MarkedBlock request before the helper thread releases what it is holding and shuts down"_s) \
    v(Bool, scribbleFreeCells, false, Normal, nullptr) \
    v(Bool, decommitUnusedMarkedBlockPages, true, Normal, "after sweeping a MarkedBlock, return its interior OS pages that hold no live cell to the OS (only where OS pages are smaller than a MarkedBlock)") \
    v(Bool, evacuateAuxiliaryBlocksAfterEveryFullCollection, false, Normal, "testing: evacuate every Auxiliary block after each full collection, wherever the mutator happens to be, and scribble the old copies"_s) \
    v(Bool, poisonDecommittedMarkedBlockPages, false, Normal, "testing: make any read of a decommitted MarkedBlock page before the block's next sweep to a free list fail (ASan poison; without ASan a 0xbd fill)"_s) \
    v(Bool, decommitUnusedMarkedBlockPagesAfterEdenCollections, false, Normal, "also do it for blocks swept after an eden collection (mostly young blocks that are refilled straight away)") \
    v(Bool, releaseIdleRegExpCodeWhenShrinkingFootprint, false, Normal, "VM::shrinkFootprintNow(KeepCodeInUse) also drops the compiled code of RegExps that have not matched since the last full collection began"_s) \
    v(Double, sizeClassProgression, 1.4, Normal, nullptr) \
    v(Unsigned, preciseAllocationCutoff, 100000, Normal, nullptr) \
    v(Bool, dumpSizeClasses, false, Normal, nullptr) \
    v(Bool, stealEmptyBlocksFromOtherAllocators, true, Normal, nullptr) \
    v(Bool, eagerlyUpdateTopCallFrame, false, Normal, nullptr) \
    v(Bool, dumpZappedCellCrashData, false, Normal, nullptr) \
    \
    v(Bool, useOSREntryToDFG, true, Normal, nullptr) \
    v(Bool, useOSREntryToFTL, true, Normal, nullptr) \
    \
    v(Bool, useFTLJIT, true, Normal, "allows the FTL JIT to be used if true"_s) \
    v(Bool, validateFTLOSRExitLiveness, false, Normal, nullptr) \
    v(Bool, poisonDeadOSRExitVariables, ASSERT_ENABLED, Normal, "Put an unmapped cell-like pointer (poisonedDeadOSRExitValue) into dead OSR exit values rather than jsUndefined, so accidental reads of dead variables crash at the access site"_s) \
    v(Unsigned, defaultB3OptLevel, 2, Normal, nullptr) \
    v(Bool, b3AlwaysFailsBeforeCompile, false, Normal, nullptr) \
    v(Bool, b3AlwaysFailsBeforeLink, false, Normal, nullptr) \
    v(Bool, validateSerializedValue, false, Normal, nullptr) /* tests CloneSerializer/Deserializer */ \
    v(Bool, ftlCrashes, false, Normal, nullptr) /* fool-proof way of checking that you ended up in the FTL. ;-) */\
    v(Bool, clobberAllRegsInFTLICSlowPath, ASSERT_ENABLED, Normal, nullptr) \
    v(Bool, useJITDebugAssertions, ASSERT_ENABLED, Normal, nullptr) \
    v(Bool, useAccessInlining, true, Normal, nullptr) \
    v(Unsigned, maxAccessVariantListSize, 8, Normal, nullptr) \
    v(Double, thresholdForUndesiredMegamorphicAccessVariantListSize, 0.5, Normal, nullptr) \
    v(Bool, usePolyvariantDevirtualization, true, Normal, nullptr) \
    v(Bool, usePolymorphicAccessInlining, true, Normal, nullptr) \
    v(Unsigned, maxPolymorphicAccessInliningListSize, 8, Normal, nullptr) \
    v(Bool, usePolymorphicCallInlining, true, Normal, nullptr) \
    v(Bool, usePolymorphicCallInliningForNonStubStatus, false, Normal, nullptr) \
    v(Unsigned, maxPolymorphicCallVariantListSize, 8, Normal, nullptr) \
    v(Unsigned, maxPolymorphicCallVariantListSizeForTopTier, 5, Normal, nullptr) \
    v(Unsigned, maxPolymorphicCallVariantListSizeForWasmToJS, 5, Normal, nullptr) \
    v(Unsigned, maxPolymorphicCallVariantsForInlining, 5, Normal, nullptr) \
    v(Unsigned, frequentCallThreshold, 2, Normal, nullptr) \
    v(Double, minimumCallToKnownRate, 0.51, Normal, nullptr) \
    v(Bool, createPreHeaders, true, Normal, nullptr) \
    v(Bool, useMovHintRemoval, true, Normal, nullptr) \
    v(Bool, usePutStackSinking, true, Normal, nullptr) \
    v(Bool, useObjectAllocationSinking, true, Normal, nullptr) \
    v(Bool, verboseObjectAllocationSinking, false, Normal, nullptr) \
    v(Bool, useValueRepElimination, true, Normal, nullptr) \
    v(Bool, useArityFixupInlining, true, Normal, nullptr) \
    v(Bool, logExecutableAllocation, false, Normal, nullptr) \
    v(Unsigned, maxDFGNodesInBasicBlockForPreciseAnalysis, 20000, Normal, "Disable precise but costly analysis and give conservative results if the number of DFG nodes in a block exceeds this threshold"_s) \
    \
    v(Bool, useConcurrentJIT, true, Normal, "allows the DFG / FTL compilation in threads other than the executing JS thread"_s) \
    v(Unsigned, minNumberOfWorklistThreads, computeNumberOfWorkerThreads(3, 2), Normal, nullptr) \
    v(Unsigned, maxNumberOfWorklistThreads, computeNumberOfWorkerThreads(3, 2), Normal, nullptr) \
    v(Unsigned, numberOfBaselineCompilerThreads, computeNumberOfWorkerThreads(3, 2), Normal, nullptr) \
    v(Unsigned, numberOfDFGCompilerThreads, computeNumberOfWorkerThreads(3, 2) - 1, Normal, nullptr) \
    v(Unsigned, numberOfFTLCompilerThreads, computeNumberOfWorkerThreads(MAXIMUM_NUMBER_OF_FTL_COMPILER_THREADS, 2) - 1, Normal, nullptr) \
    v(Unsigned, numberOfWasmCompilerThreads, computeNumberOfWorkerThreads(INT32_MAX, 2) - 1, Normal, nullptr) \
    v(Unsigned, worklistLoadFactor, 1, Normal, nullptr) \
    v(Unsigned, worklistBaselineLoadWeight, 1, Normal, nullptr) \
    v(Unsigned, worklistDFGLoadWeight, 1, Normal, nullptr) \
    v(Unsigned, worklistFTLLoadWeight, 1, Normal, nullptr) \
    v(Int32, priorityDeltaOfDFGCompilerThreads, computePriorityDeltaOfWorkerThreads(-1, 0), Normal, nullptr) \
    v(Int32, priorityDeltaOfFTLCompilerThreads, computePriorityDeltaOfWorkerThreads(-2, 0), Normal, nullptr) \
    v(Int32, priorityDeltaOfWasmCompilerThreads, computePriorityDeltaOfWorkerThreads(-1, 0), Normal, nullptr) \
    \
    v(Bool, useProfiler, false, Normal, nullptr) \
    v(Bool, dumpProfilerDataAtExit, false, Normal, nullptr) \
    v(Bool, disassembleBaselineForProfiler, true, Normal, nullptr) \
    v(Unsigned, abbreviateSourceCodeForProfiler, 0, Normal, nullptr) \
    \
    v(Bool, useArchitectureSpecificOptimizations, true, Normal, nullptr) \
    \
    v(Bool, breakOnThrow, false, Normal, nullptr) \
    \
    v(Unsigned, maximumOptimizationCandidateBytecodeCost, 100000, Normal, nullptr) \
    v(Unsigned, maximumCachedAssemblerBufferSize, 1 * MB, Normal, "Assembler scratch buffers larger than this are freed after compilation instead of being cached per thread (0 = cache any size)"_s) \
    \
    v(Unsigned, maximumFunctionForCallInlineCandidateBytecodeCostForDFG, 80, Normal, nullptr) \
    v(Unsigned, maximumFunctionForClosureCallInlineCandidateBytecodeCostForDFG, 80, Normal, nullptr) \
    v(Unsigned, maximumFunctionForConstructInlineCandidateBytecodeCostForDFG, 80, Normal, nullptr) \
    v(Unsigned, maximumFunctionForCallInlineCandidateBytecodeCostForFTL, 170, Normal, nullptr) \
    v(Unsigned, maximumFunctionForClosureCallInlineCandidateBytecodeCostForFTL, 100, Normal, nullptr) \
    v(Unsigned, maximumFunctionForConstructInlineCandidateBytecodeCostForFTL, 100, Normal, nullptr) \
    \
    v(Unsigned, maximumFTLCandidateBytecodeCost, 60000, Normal, nullptr) \
    \
    v(Double, ratioFTLNodesToBytecodeCost, 1.9, Normal, "Ratio converting FTL # of DFG nodes to approx bytecode cost") \
    \
    /* Depth of inline stack, so 1 = no inlining, 2 = one level, etc. */ \
    v(Unsigned, maximumInliningDepth, 5, Normal, "maximum allowed inlining depth.  Depth of 1 means no inlining"_s) \
    v(Unsigned, maximumInliningRecursion, 2, Normal, nullptr) \
    \
    /* Maximum size of a caller for enabling inlining. This is purely to protect us */\
    /* from super long compiles that take a lot of memory. */\
    v(Unsigned, maximumInliningCallerBytecodeCost, 10000, Normal, nullptr) \
    \
    v(Bool, useGlobalInliningPlanner, true, Normal, "Survey and rank every inlining candidate before parsing and spend one compilation-wide budget on the best of them, instead of deciding each call site in bytecode order"_s) \
    v(Unsigned, globalInliningPlanBudgetForDFG, 2500, Normal, "Total callee bytecode cost the DFG may plan to inline in one compilation"_s) \
    v(Unsigned, globalInliningPlanBudgetForFTL, 12000, Normal, "Total callee bytecode cost the FTL may plan to inline in one compilation"_s) \
    v(Unsigned, maximumGlobalInliningPlanSites, 20000, Normal, "Cap on how many call sites one inlining plan will survey"_s) \
    v(Double, inliningPlanTierBonusBase, 2.0, Normal, "Multiplicative benefit per tier the callee has reached (LLInt, Baseline, DFG, FTL) when ranking inlining candidates"_s) \
    v(Double, inliningPlanTierBonusPowerForFTL, 3.0, Normal, "Base for the bonus multiplier for FTL callees"_s) \
    v(Double, inliningPlanTierBonusPowerForDFG, 2.0, Normal, "Base for the bonus multiplier for DFG callees"_s) \
    v(Double, inliningPlanTierBonusPowerForBaseline, 1.0, Normal, "Base for the bonus multiplier for Baseline callees"_s) \
    v(Double, inliningPlanDepthPenalty, 1.5, Normal, "Divisive benefit penalty per level of inline-stack nesting when ranking inlining candidates"_s) \
    \
    v(Unsigned, maximumVarargsForInlining, 100, Normal, nullptr) \
    \
    v(Unsigned, maximumBinaryStringSwitchCaseLength, 50, Normal, nullptr) \
    v(Unsigned, maximumBinaryStringSwitchTotalLength, 2000, Normal, nullptr) \
    v(Unsigned, maximumInlineStringSwitchCaseCount, 64, Normal, "Maximum number of cases for which the baseline JIT dispatches op_switch_string inline instead of calling out."_s) \
    v(Unsigned, maximumRegExpTestInlineCodesize, 500, Normal, "Maximum code size in bytes for inlined RegExp.test JIT code."_s) \
    v(Unsigned, maximumRegExpJITCodeSize, 16 * MB, Normal, "Maximum generated code size in bytes for RegExp JIT compilation before falling back to the interpreter."_s) \
    \
    v(Unsigned, wasmInliningMaximumDepth, 7, Normal, "Maximum inlining depth to consider inlining a wasm function."_s) \
    v(Unsigned, wasmInliningMaximumWasmCalleeSize, 500, Normal, "Maximum wasm size in bytes to consider inlining a wasm function."_s) \
    v(Unsigned, wasmInliningMaximumCount, 60, Normal, "Maximum inlining count to consider inlining a wasm function."_s) \
    v(Unsigned, wasmInliningMinimumBudget, 50, Normal, "Minimum budget for which the wasmInliningFactor does not apply"_s) \
    v(Unsigned, wasmInliningFactor, 5, Normal, "Maximum multiple budget in comparison to initial wasm size"_s) \
    v(Unsigned, wasmInliningBudget, 6000, Normal, "Maximum budget that allows inlining more"_s) \
    v(Double, wasmInliningLargeFunctionGrowthFactor, 1.4, Normal, "Minimum growth factor (multiplied by initial wasm size) that bounds the large-function inlining budget."_s) \
    v(Unsigned, wasmInliningTinyFunctionThreshold, 12, Normal, "Wasm size threshold for tiny wasm functions"_s) \
    v(Unsigned, wasmInliningSmallFunctionThreshold, 50, Normal, "Wasm size threshold for small wasm functions"_s) \
    \
    v(Double, jitPolicyScale, 1.0, Normal, "scale JIT thresholds to this specified ratio between 0.0 (compile ASAP) and 1.0 (compile like normal)."_s) \
    v(Int32, numberOfSuperAndPerformanceCoresOverride, 0, Normal, "If non-zero, overrides the number of Super and Performance (i.e. non-Efficiency) cores reported by the hardware; 0 means use the value reported by the hardware."_s) \
    v(Double, dfgThresholdScaleForFewPerformanceCores, 2.0, Normal, "On Apple silicon Macs with few Super and Performance cores, scale the DFG tier-up thresholds (thresholdForOptimize*) by this factor."_s) \
    v(Double, ftlThresholdScaleForFewPerformanceCores, 1.5, Normal, "On Apple silicon Macs with few Super and Performance cores, scale the FTL tier-up thresholds (thresholdForFTLOptimize*) by this factor."_s) \
    v(Bool, forceEagerCompilation, false, Normal, nullptr) \
    v(Double, startupJITDeferralScale, 1, Normal, "While the VM's startup window is active, LLInt->Baseline and Baseline->DFG tier-up require this multiple of the normal execution-count threshold (1 = off)."_s) \
    v(Int32, thresholdForJITAfterWarmUp, 500, Normal, nullptr) \
    v(Int32, thresholdForJITSoon, 100, Normal, nullptr) \
    \
    v(Int32, thresholdForOptimizeAfterWarmUp, 1000, Normal, nullptr) \
    v(Int32, thresholdForOptimizeAfterLongWarmUp, 1000, Normal, nullptr) \
    v(Int32, thresholdForOptimizeSoon, 1000, Normal, nullptr) \
    v(Int32, executionCounterIncrementForLoop, 1, Normal, nullptr) \
    v(Int32, executionCounterIncrementForEntry, 15, Normal, nullptr) \
    \
    v(Int32, thresholdForFTLOptimizeAfterWarmUp, 64000, Normal, nullptr) \
    v(Int32, thresholdForFTLOptimizeSoon, 1000, Normal, nullptr) \
    v(Int32, ftlTierUpCounterIncrementForLoop, 1, Normal, nullptr) \
    v(Int32, ftlTierUpCounterIncrementForReturn, 15, Normal, nullptr) \
    v(Unsigned, ftlOSREntryFailureCountForReoptimization, 15, Normal, nullptr) \
    v(Unsigned, ftlOSREntryRetryThreshold, 100, Normal, nullptr) \
    \
    v(Int32, evalThresholdMultiplier, 10, Normal, nullptr) \
    v(Unsigned, maximumEvalCacheableSourceLength, 256, Normal, nullptr) \
    \
    v(Int32, maximumExecutionCountsBetweenCheckpointsForBaseline, 1000, Normal, nullptr) \
    v(Int32, maximumExecutionCountsBetweenCheckpointsForUpperTiers, 30000, Normal, nullptr) \
    v(Int32, highCostBaselineProfilingFunctionBytecodeCost, 10000, Normal, nullptr) \
    v(Int32, valueProfileFillingRateMonitoringBytecodeCost, 5000, Normal, nullptr) \
    \
    v(Unsigned, likelyToTakeSlowCaseMinimumCount, 20, Normal, nullptr) \
    v(Unsigned, couldTakeSlowCaseMinimumCount, 10, Normal, nullptr) \
    \
    v(Unsigned, osrExitCountForReoptimization, 100, Normal, nullptr) \
    v(Unsigned, osrExitCountForReoptimizationFromLoop, 5, Normal, nullptr) \
    \
    v(Unsigned, reoptimizationRetryCounterMax, 0, Normal, nullptr)  \
    v(Unsigned, weakReferenceJettisonReoptimizationLimit, 4, Normal, "Optimized code jettisoned because a cell it references weakly died counts toward the reoptimization back-off of its function, like code jettisoned for exiting too often, while the function's reoptimization retry counter is below this value. 0: it never counts."_s) \
    \
    v(Unsigned, minimumOptimizationDelay, 1, Normal, nullptr) \
    v(Unsigned, maximumOptimizationDelay, 5, Normal, nullptr) \
    v(Double, desiredProfileLivenessRate, 0.75, Normal, nullptr) \
    v(Double, desiredProfileFullnessRate, 0.35, Normal, nullptr) \
    \
    v(Double, quickDFGTierUpThresholdFactor, defaultQuickDFGTierUpThresholdFactor(), Normal, "Threshold factor for quick DFG tier-up"_s) \
    v(Double, relaxedProfileCoverageFactorForQuickDFGTierUp, defaultRelaxedProfileCoverageFactorForQuickDFGTierUp(), Normal, "Profile coverage scaling factor for quick DFG tier-up"_s) \
    v(Double, quickFTLTierUpThresholdFactor, defaultQuickFTLTierUpThresholdFactor(), Normal, "Threshold factor for quick FTL tier-up"_s) \
    \
    v(Double, doubleVoteRatioForDoubleFormat, 2, Normal, nullptr) \
    v(Double, structureCheckVoteRatioForHoisting, 1, Normal, nullptr) \
    v(Double, checkArrayVoteRatioForHoisting, 1, Normal, nullptr) \
    \
    v(Unsigned, maximumDirectCallStackSize, 200, Normal, nullptr) \
    \
    v(Unsigned, minimumNumberOfScansBetweenRebalance, 100, Normal, nullptr) \
    v(Unsigned, numberOfGCMarkers, computeNumberOfGCMarkers(8), Normal, nullptr) \
    v(Bool, useParallelMarkingConstraintSolver, true, Normal, nullptr) \
    v(Unsigned, opaqueRootMergeThreshold, 1000, Normal, nullptr) \
    v(Unsigned, maxHeapSizeAsRAMSizeMultiple, 0, Normal, nullptr) \
    v(Double, minHeapUtilization, 0.8, Normal, nullptr) \
    v(Double, minMarkedBlockUtilization, 0.9, Normal, nullptr) \
    v(Unsigned, slowPathAllocsBetweenGCs, 0, Normal, "force a GC on every Nth slow path alloc, where N is specified by this option"_s) \
    /* WARNING: this option is important for compatibility be *VERY* careful when lowering it. See: rdar://145585141 and https://bugs.webkit.org/show_bug.cgi?id=289330 */ \
    /* Cap on the bytecode RegExp interpreter's backtracking-context pool (allocated a page at a time, only while a match runs, and released after it). Each quantified-group iteration takes a context sized by the pattern's call frame, which grew with the ParenthesesOnce/BackReference slots and the alternation folding; 192MB keeps the reach interpreter-only patterns had at 128MB (prism/highlight.js tokenizers over ~600KB inputs). */ \
    v(Unsigned, maxRegExpStackSize, 192 * MB, Normal, nullptr) \
    \
    v(Double, percentCPUPerMBForFullTimer, 0.0003125, Normal, nullptr) \
    v(Double, percentCPUPerMBForEdenTimer, 0.0025, Normal, nullptr) \
    v(Double, collectionTimerMaxPercentCPU, 0.10, Normal, nullptr) \
    \
    v(Bool, forceWeakRandomSeed, false, Normal, nullptr) \
    v(Unsigned, forcedWeakRandomSeed, 0, Normal, nullptr) \
    \
    v(Bool, alwaysHaveABadTime, false, Normal, "debugging option to test HaveABadTime mode"_s) \
    v(Bool, allowDoubleShape, true, Normal, "debugging option to test disabling use of DoubleShape"_s) \
    v(Bool, useZombieMode, false, Normal, "debugging option to scribble over dead objects with 0xbadbeef0"_s) \
    v(Bool, useImmortalObjects, false, Normal, "debugging option to keep all objects alive forever"_s) \
    v(Bool, sweepSynchronously, false, Normal, "debugging option to sweep all dead objects synchronously at GC end before resuming mutator"_s) \
    v(Unsigned, maxSingleAllocationSize, 0, Configurable, "debugging option to limit individual allocations to a max size (0 = limit not set, N = limit size in bytes)"_s) \
    \
    v(GCLogLevel, logGC, GCLogging::None, Normal, "debugging option to log GC activity (0 = None, 1 = Basic, 2 = Verbose)"_s) \
    v(Bool, useGC, true, Normal, nullptr) \
    v(Bool, useGlobalGC, false, Normal, nullptr) \
    v(Bool, gcAtEnd, false, Normal, "If true, the jsc CLI will do a GC before exiting"_s) \
    v(Bool, forceGCSlowPaths, false, Normal, "If true, we will force all JIT fast allocations down their slow paths."_s) \
    v(Bool, forceDidDeferGCWork, false, Normal, "If true, we will force all DeferGC destructions to perform a GC."_s) \
    v(Unsigned, gcMaxHeapSize, 0, Normal, nullptr) \
    v(Size, forceRAMSize, 0, Normal, nullptr) \
    v(Bool, recordGCPauseTimes, false, Normal, nullptr) \
    v(Bool, dumpHeapStatisticsAtVMDestruction, false, Normal, nullptr) \
    v(Bool, enableStrongRefTracker, false, Normal, "Enable logging of live Strong<*> values. Use alongside $vm.triggerMemoryPressure() and dumpHeapOnLowMemory."_s) \
    v(Bool, dumpHeapOnLowMemory, false, Normal, "Dump a heap dump when the memory handler is triggered. Use alongside $vm.triggerMemoryPressure() and enableStrongRefTracker."_s) \
    v(Bool, forceCodeBlockToJettisonDueToOldAge, false, Normal, "If true, this means that anytime we can jettison a CodeBlock due to old age, we do."_s) \
    v(Bool, useEagerCodeBlockJettisonTiming, false, Normal, "If true, the time slices for jettisoning a CodeBlock due to old age are shrunk significantly."_s) \
    FOR_EACH_JSC_CODEBLOCK_AGING_OPTION(v) \
    FOR_EACH_JSC_BYTECODE_CACHE_DECODER_OPTION(v) \
    \
    v(Bool, useTypeProfiler, false, Normal, nullptr) \
    v(Bool, useControlFlowProfiler, false, Normal, nullptr) \
    \
    v(Bool, useSamplingProfiler, false, Normal, nullptr) \
    v(Unsigned, sampleInterval, 1000, Normal, "Time between stack traces in microseconds."_s) \
    v(Bool, collectExtraSamplingProfilerData, false, Normal, "This corresponds to the JSC shell's --sample option, or if we're wanting to use the sampling profiler via the Debug menu in the browser."_s) \
    v(Unsigned, samplingProfilerTopFunctionsCount, 12, Normal, "Number of top functions to report when using the command line interface."_s) \
    v(Unsigned, samplingProfilerTopBytecodesCount, 40, Normal, "Number of top bytecodes to report when using the command line interface."_s) \
    v(Bool, samplingProfilerIgnoreExternalSourceID, false, Normal, "Ignore external source ID when aggregating results from sampling profiler"_s) \
    v(OptionString, samplingProfilerPath, nullptr, Normal, "The path to the directory to write sampiling profiler output to. This probably will not work with WK2 unless the path is in the sandbox."_s) \
    v(Bool, sampleCCode, false, Normal, "Causes the sampling profiler to record profiling data for C frames."_s) \
    \
    v(Bool, alwaysGeneratePCToCodeOriginMap, false, Normal, "This will make sure we always generate a PCToCodeOriginMap for JITed code."_s) \
    \
    v(Double, randomIntegrityAuditRate, 0.05, Normal, "Probability of random integrity audits [0.0 - 1.0]"_s) \
    v(Bool, verifyGC, false, Normal, nullptr) \
    v(Bool, verboseVerifyGC, false, Normal, nullptr) \
    v(Bool, verifyHeap, false, Normal, nullptr) \
    v(Unsigned, numberOfGCCyclesToRecordForVerification, 3, Normal, nullptr) \
    \
    v(Unsigned, exceptionStackTraceLimit, 100, Normal, "Stack trace limit for internal Exception object"_s) \
    v(Unsigned, defaultErrorStackTraceLimit, 100, Normal, "The default value for Error.stackTraceLimit"_s) \
    v(Bool, exitOnResourceExhaustion, false, Normal, nullptr) \
    v(Bool, useExceptionFuzz, false, Normal, nullptr) \
    v(Unsigned, fireExceptionFuzzAt, 0, Normal, nullptr) \
    v(Bool, fuzzAtomicJITMemcpy, false, Normal, nullptr) \
    v(Bool, validateDFGExceptionHandling, ASSERT_ENABLED, Normal, "Causes the DFG to emit code validating exception handling for each node that can exit"_s) \
    v(Bool, dumpSimulatedThrows, false, Normal, "Dumps the call stack of the last simulated throw if exception scope verification fails"_s) \
    v(Bool, validateExceptionChecks, false, Normal, "Verifies that needed exception checks are performed."_s) \
    v(Unsigned, unexpectedExceptionStackTraceLimit, 100, Normal, "Stack trace limit for debugging unexpected exceptions observed in the VM"_s) \
    \
    v(Bool, validateDFGClobberize, false, Normal, "Emits code in the DFG/FTL to validate the Clobberize phase"_s) \
    v(Bool, validateBoundsCheckElimination, false, Normal, "Emits code in the DFG/FTL to validate bounds check elimination"_s) \
    v(Bool, validateDFGMayExit, ASSERT_ENABLED, Normal, "Emits code in the DFG/FTL to validate the MayExit phase"_s) \
    \
    v(Bool, validateVMEntryCalleeSaves, false, Configurable, "Causes vmEntryToJavaScript to validate VMEntry callee saves are properly restored"_s) \
    \
    v(Bool, useExecutableAllocationFuzz, false, Normal, nullptr) \
    v(Unsigned, fireExecutableAllocationFuzzAt, 0, Normal, nullptr) \
    v(Unsigned, fireExecutableAllocationFuzzAtOrAfter, 0, Normal, nullptr) \
    v(Bool, fireExecutableAllocationFuzzRandomly, false, Normal, nullptr) \
    v(Double, fireExecutableAllocationFuzzRandomlyProbability, 0.1, Normal, nullptr) \
    v(Bool, verboseExecutableAllocationFuzz, false, Normal, nullptr) \
    v(Bool, zeroExecutableMemoryOnFree, false, Normal, "0 out instructions when freeing JIT memory."_s) \
    \
    v(Bool, useOSRExitFuzz, false, Normal, nullptr) \
    v(Unsigned, fireOSRExitFuzzAtStatic, 0, Normal, nullptr) \
    v(Unsigned, fireOSRExitFuzzAt, 0, Normal, nullptr) \
    v(Unsigned, fireOSRExitFuzzAtOrAfter, 0, Normal, nullptr) \
    v(Bool, verboseOSRExitFuzz, true, Normal, nullptr) \
    \
    /* LOL options */ \
    v(Bool, useLOLJIT, false, Normal, "Use LOL instead of Baseline"_s) \
    v(Bool, verboseLOLAllocation, false, Normal, "Log info about LOL's register allocation state"_s) \
    \
    v(Unsigned, seedOfVMRandomForFuzzer, 0, Normal, "0 means not fuzzing this; use a cryptographically random seed"_s) \
    v(Bool, useRandomizingFuzzerAgent, false, Normal, nullptr) \
    v(Unsigned, seedOfRandomizingFuzzerAgent, 1, Normal, nullptr) \
    v(Bool, dumpFuzzerAgentPredictions, false, Normal, nullptr) \
    v(Bool, useDoublePredictionFuzzerAgent, false, Normal, nullptr) \
    v(Bool, useFileBasedFuzzerAgent, false, Normal, nullptr) \
    v(Bool, usePredictionFileCreatingFuzzerAgent, false, Normal, nullptr) \
    v(Bool, requirePredictionForFileBasedFuzzerAgent, false, Normal, nullptr) \
    v(OptionString, fuzzerPredictionsFile, nullptr, Normal, "file with list of predictions for FileBasedFuzzerAgent"_s) \
    v(Bool, useNarrowingNumberPredictionFuzzerAgent, false, Normal, nullptr) \
    v(Bool, useWideningNumberPredictionFuzzerAgent, false, Normal, nullptr) \
    \
    v(Bool, logPhaseTimes, false, Normal, nullptr) \
    v(Double, rareBlockPenalty, 0.001, Normal, nullptr) \
    v(Bool, airGreedyRegAllocVerbose, false, Normal, nullptr) \
    v(OptionString, airGreedyRegAllocDumpFunction, nullptr, Normal, "dump greedy register allocator state and IR for functions matching this substring"_s) \
    v(Double, airGreedyRegAllocSplitMultiplier, 2.0, Normal, nullptr) \
    v(Bool, airGreedyRegAllocSplitAroundLoops, false, Normal, nullptr) \
    v(Double, airGreedyRegAllocLoopSplitMaxLoopFraction, 0.75, Normal, nullptr) \
    v(Bool, airGreedyRegAllocSpillsEverything, false, Normal, nullptr) \
    v(Bool, airDumpPhaseStats, false, Normal, nullptr) \
    v(Bool, airValidateGreedRegAlloc, ASSERT_ENABLED, Normal, nullptr) \
    v(Bool, airRandomizeRegs, false, Normal, nullptr) \
    v(Unsigned, airRandomizeRegsSeed, 0, Normal, nullptr) \
    v(Bool, coalesceSpillSlots, true, Normal, nullptr) \
    v(Bool, logAirRegisterPressure, false, Normal, nullptr) \
    v(Bool, useB3TailDup, true, Normal, nullptr) \
    v(Unsigned, maxB3TailDupBlockSize, 3, Normal, nullptr) \
    v(Unsigned, maxB3TailDupBlockSuccessors, 3, Normal, nullptr) \
    v(Bool, useB3HoistLoopInvariantValues, true, Normal, nullptr) \
    v(Bool, useB3CanonicalizePrePostIncrements, false, Normal, nullptr) \
    v(Bool, useB3EliminateWasmGCAllocations, true, Normal, "eliminate non-escaping wasm-GC struct allocations in B3"_s) \
    v(Bool, useB3ReduceStrengthFixpoint, false, Normal, "iterate B3 reduceStrength to a fixpoint instead of a single pass (for debugging)"_s) \
    v(Bool, useAirOptimizePairedLoadStore, true, Normal, nullptr) \
    \
    v(Bool, useDollarVM, false, Restricted, "installs the $vm debugging tool in global objects"_s) \
    v(OptionString, functionOverrides, nullptr, Restricted, "file with debugging overrides for function bodies"_s) \
    \
    v(Unsigned, watchdog, 0, Normal, "watchdog timeout (0 = Disabled, N = a timeout period of N milliseconds)"_s) \
    v(Bool, usePollingTraps, false, Normal, "use polling (instead of signalling) VM traps"_s) \
    v(Bool, forceTrapAwareStackChecks, false, Normal, "force trap aware stack checks to be taken for testing"_s) \
    \
    v(Bool, useMachForExceptions, true, Normal, "Use mach exceptions rather than signals to handle faults and pass thread messages. (This does nothing on platforms without mach)"_s) \
    v(Bool, allowNonSPTagging, true, Normal, "allow use of the pacib instruction instead of just pacibsp (This can break lldb/posix signals as it puts live data below SP)"_s) \
    \
    v(Bool, useICStats, false, Normal, nullptr) \
    \
    v(Bool, useFuzzerMode, false, Normal, nullptr) \
    \
    v(Unsigned, prototypeHitCountForLLIntCaching, 2, Normal, "Number of prototype property hits before caching a prototype in the LLInt. A count of 0 means never cache."_s) \
    \
    v(Bool, dumpCompiledRegExpPatterns, false, Normal, nullptr) \
    v(Bool, verboseRegExpCompilation, false, Normal, nullptr) \
    \
    v(Bool, dumpModuleRecord, false, Normal, nullptr) \
    v(Bool, dumpModuleLoadingState, false, Normal, nullptr) \
    v(Bool, exposeInternalModuleLoader, false, Normal, "expose the internal module loader object to the global space for debugging"_s) \
    \
    v(Bool, exposePrivateIdentifiers, false, Normal, "Allow non-builtin scripts to use private identifiers. Mostly useful to expose @superSamplerBegin/End intrinsics for profiling"_s) \
    \
    v(Bool, useSuperSampler, false, Normal, nullptr) \
    \
    v(Bool, useSourceProviderCache, true, Normal, "If false, the parser will not use the source provider cache. It's good to verify everything works when this is false. Because the cache is so successful, it can mask bugs."_s) \
    v(Bool, useCodeCache, true, Normal, "If false, the unlinked byte code cache will not be used."_s) \
    \
    v(Bool, useWasm, canUseWasm(), Normal, "Expose the Wasm global object."_s) \
    \
    v(Bool, failToCompileWasmCode, false, Normal, "If true, no Wasm::Plan will sucessfully compile a function."_s) \
    v(Size, wasmSmallPartialCompileLimit, 5000, Normal, "Limit on the number of bytes a Wasm::Plan::compile should attempt for small wasm binary before checking for other work."_s) \
    v(Size, wasmLargePartialCompileLimit, 20000, Normal, "Limit on the number of bytes a Wasm::Plan::compile should attempt for large wasm binary before checking for other work."_s) \
    v(Unsigned, wasmOMGOptimizationLevel, Options::defaultB3OptLevel(), Normal, "B3 Optimization level for OMG Web Assembly module compilations."_s) \
    v(Bool, useWasmByteLoopReplacement, true, Normal, "If true, OMG replaces a loop that copies or fills linear memory one byte per iteration with the equivalent bulk memory operation."_s) \
    \
    v(Bool, useBBQTierUpChecks, true, Normal, "Enables tier up checks for our BBQ code."_s) \
    v(Bool, useWasmOSR, true, Normal, nullptr) \
    v(Int32, thresholdForBBQOptimizeAfterWarmUp, 150, Normal, "The count before we tier up a function to BBQ."_s) \
    v(Int32, thresholdForBBQOptimizeSoon, 50, Normal, nullptr) \
    v(Int32, thresholdForOMGOptimizeAfterWarmUp, 50000, Normal, "The count before we tier up a function to OMG."_s) \
    v(Int32, thresholdForOMGOptimizeSoon, 500, Normal, nullptr) \
    v(Unsigned, maximumOMGCandidateCost, 100000, Normal, nullptr) \
    v(Int32, omgTierUpCounterIncrementForLoop, 1, Normal, "The amount the tier up counter is incremented on each loop backedge."_s) \
    v(Int32, omgTierUpCounterIncrementForEntry, 15, Normal, "The amount the tier up counter is incremented on each function entry."_s) \
    v(Int32, wasmOMGEntryIncrementSizeReference, 128, Normal, "If non-zero, the BBQ->OMG function-entry tier-up increment is scaled down for functions whose bytecode size is below this reference (work-proportional tier-up): increment = clamp(entryIncrement * size / reference, 1, entryIncrement). 0 disables (flat increment)."_s) \
    v(Bool, useWasmFastMemory, true, Normal, "If true, we will try to use a 32-bit address space with a signal handler to bounds check wasm memory."_s) \
    v(Bool, logWasmMemory, false, Normal, nullptr) \
    v(Unsigned, wasmFastMemoryRedzonePages, 128, Normal, "Wasm fast memories use 4GiB virtual allocations, plus a redzone (counted as multiple of 64KiB Wasm pages) at the end to catch reg+imm accesses which exceed 32-bit, anything beyond the redzone is explicitly bounds-checked"_s) \
    v(Bool, crashIfWasmCantFastMemory, false, Normal, "If true, we will crash if we can't obtain fast memory for wasm."_s) \
    v(Bool, crashOnFailedWasmValidate, false, Normal, "If true, we will crash if we can't validate a wasm module instead of throwing an exception."_s) \
    v(Unsigned, maxNumWasmFastMemories, hasCapacityToUseLargeGigacage() ? 8 : 3, Normal, nullptr) \
    v(Bool, verboseBBQJITAllocation, false, Normal, "Logs extra information about register allocation during BBQ JIT"_s) \
    v(Bool, verboseBBQJITInstructions, false, Normal, "Logs instruction information during BBQ JIT"_s) \
    v(Bool, disableBBQConsts, false, Normal, "Wasm <type>.const instructions in BBQ JIT won't lower to a const BBQ::Value"_s) \
    v(Bool, useBBQJIT, true, Normal, "allows the BBQ JIT to be used if true"_s) \
    v(Bool, useOMGJIT, true, Normal, "allows the OMG JIT to be used if true"_s) \
    v(OptionRange, wasmFunctionIndexRangeToCompile, nullptr, Normal, "wasm function index range to allow compilation on, e.g. 1:100"_s) \
    v(Bool, useEagerWasmModuleHashing, false, Normal, "Unnamed Wasm modules are identified in backtraces through their hash, if available."_s) \
    v(Bool, useArrayAllocationProfiling, true, Normal, "If true, we will use our normal array allocation profiling. If false, the allocation profile will always claim to be undecided."_s) \
    v(Bool, forcePolyProto, false, Normal, "If true, create_this will always create an object with a poly proto structure."_s) \
    v(Bool, forceMiniVMMode, false, Normal, "If true, it will force mini VM mode on."_s) \
    v(Bool, useMiniVMModeWithoutJIT, true, Normal, "Having no JIT is taken to mean that memory matters more than speed. Not so where the code comes compiled."_s) \
    v(Bool, useTracePoints, false, Normal, nullptr) \
    v(Bool, useCompilerSignpost, false, Normal, nullptr) \
    v(Bool, useGCSignpost, false, Normal, nullptr) \
    v(Bool, traceLLIntExecution, false, Configurable, nullptr) \
    v(Bool, traceLLIntSlowPath, false, Configurable, nullptr) \
    v(Bool, traceBaselineJITExecution, false, Normal, nullptr) \
    v(Unsigned, thresholdForGlobalLexicalBindingEpoch, UINT_MAX, Normal, "Threshold for global lexical binding epoch. If the epoch reaches to this value, CodeBlock metadata for scope operations will be revised globally. It needs to be greater than 1."_s) \
    v(OptionString, diskCachePath, nullptr, Restricted, nullptr) \
    v(Bool, verboseDiskCache, false, Normal, "If true, we will log cache hits and misses."_s) \
    v(Bool, forceDiskCache, false, Restricted, nullptr) \
    v(Bool, validateAbstractInterpreterState, false, Restricted, nullptr) \
    v(Double, validateAbstractInterpreterStateProbability, 0.5, Normal, nullptr) \
    v(OptionString, dumpJITMemoryPath, nullptr, Restricted, nullptr) \
    v(Double, dumpJITMemoryFlushInterval, 10, Restricted, "Maximum time in between flushes of the JIT memory dump in seconds."_s) \
    v(Bool, useUnlinkedCodeBlockJettisoning, false, Normal, "If true, UnlinkedCodeBlock can be jettisoned."_s) \
    v(Bool, forceOSRExitToLLInt, false, Normal, "If true, we always exit to the LLInt. If false, we exit to whatever is most convenient."_s) \
    v(Unsigned, getByValICMaxNumberOfIdentifiers, 4, Normal, "Number of identifiers we see in the LLInt that could cause us to bail on generating an IC for get_by_val."_s) \
    v(Bool, useRandomizingExecutableIslandAllocation, false, Normal, "For the arm64 ExecutableAllocator, if true, select which region to use randomly. This is useful for testing that jump islands work."_s) \
    v(Bool, exposeProfilersOnGlobalObject, false, Normal, "If true, we will expose functions to enable/disable both the sampling profiler and the super sampler"_s) \
    v(Bool, allowUnsupportedTiers, false, Normal, "If true, we will not disable DFG or FTL when an experimental feature is enabled."_s) \
    v(Bool, returnEarlyFromInfiniteLoopsForFuzzing, false, Normal, nullptr) \
    v(Size, earlyReturnFromInfiniteLoopsLimit, 1300000000, Normal, "When returnEarlyFromInfiniteLoopsForFuzzing is true, this determines the number of executions a loop can run for before just returning. This is helpful for the fuzzer so it doesn't get stuck in infinite loops."_s) \
    v(Bool, useLICMFuzzing, false, Normal, nullptr) \
    v(Unsigned, seedForLICMFuzzer, 424242, Normal, nullptr) \
    v(Double, allowHoistingLICMProbability, 0.5, Normal, nullptr) \
    v(Bool, exposeCustomSettersOnGlobalObjectForTesting, false, Normal, nullptr) \
    v(Bool, useJITCage, canUseJITCage(), Normal, nullptr) \
    v(Bool, useAllocationProfiling, false, Normal, "Allows toggling of bmalloc/libPAS allocation profiling features at JSC launch."_s) \
    v(Unsigned, allocationProfilingMode, 0, Normal, "Allows custom arguments to be passed to bmalloc/libPAS allocation profiling features at JSC launch."_s) \
    v(Bool, dumpBaselineJITSizeStatistics, false, Normal, nullptr) \
    v(Bool, dumpDFGJITSizeStatistics, false, Normal, nullptr) \
    v(Bool, useLoopUnrolling, true, Normal, nullptr) \
    v(Bool, usePartialLoopUnrolling, true, Normal, nullptr) \
    v(Bool, verboseLoopUnrolling, false, Normal, nullptr) \
    v(Bool, disallowLoopUnrollingForNonInnermost, true, Normal, nullptr) \
    v(Unsigned, maxLoopUnrollingCount, 5, Normal, nullptr) \
    v(Unsigned, maxLoopUnrollingBodyNodeSize, 200, Normal, nullptr) \
    v(Unsigned, maxLoopUnrollingIterationCount, 4, Normal, nullptr) \
    v(Unsigned, maxPartialLoopUnrollingBodyNodeSize, 70, Normal, nullptr) \
    v(Unsigned, maxPartialLoopUnrollingIterationCount, 4, Normal, nullptr) \
    v(Unsigned, maxNumericHotLoopSize, 225, Normal, nullptr) \
    v(Unsigned, maxIntegerRangeOptimizationRelationshipsPerNode, 24, Normal, "How many relationships IRO keeps about any one node, 0 for no cap."_s) \
    v(Unsigned, maxIntegerRangeOptimizationWork, 50000000, Normal, "Give up threshold for IRO"_s) \
    v(Bool, printEachUnrolledLoop, false, Normal, nullptr) \
    v(Bool, verboseExecutablePoolAllocation, false, Normal, nullptr) \
    v(Bool, useHandlerICInFTL, false, Normal, nullptr) \
    v(Bool, useLLIntICs, true, Normal, "Use property and call ICs in LLInt code."_s) \
    v(Bool, useLazyValueProfilePredictions, true, Normal, "Allocate the predictions of a CodeBlock's value profiles the first time one of them has something to predict instead of when the CodeBlock is linked."_s) \
    v(Int32, thresholdForValueProfilePredictions, 40, Normal, "LLInt execution count (5 per call, 10 per return, 1 per loop iteration) below which a collection leaves the samples of an interpreted CodeBlock's value profiles in their buckets instead of folding them into predictions."_s) \
    v(Bool, useLazyLLIntCallLinkInfos, true, Normal, "Allocate the CallLinkInfo and the ArrayProfile of a call site in LLInt / Baseline metadata when the site is executed for the second time (tail calls: for the first time) instead of when the CodeBlock is linked."_s) \
    v(Bool, useLazyFunctionExecutables, true, Normal, "If true, a CodeBlock creates the FunctionExecutable for a function declaration / expression the first time that new_func* executes (or before a JIT compiles the block) instead of creating all of them when the CodeBlock is linked; a module body never creates one for its heap-allocated declarations (the module environment already did)."_s) \
    v(Bool, useLazyModuleFunctionDeclarations, true, Normal, "If true, InitializeEnvironment leaves the module environment slot of a function declaration empty and the function object (and its FunctionExecutable) is created the first time the binding is read: get_from_scope with the LazyClosureVar resolve type, a by-name lookup on the module environment, or a module namespace object property access."_s) \
    v(Bool, predictFunctionForUnprofiledLazyClosureVarForTesting, false, Normal, "The DFG treats a get_from_scope<LazyClosureVar> that never ran as producing a function instead of exiting, so that tests reach the optimizing tiers' code for instantiating a function declaration."_s) \
    v(Bool, useLazyCatchLiveness, true, Normal, "If true, an op_catch that executes in the LLInt / Baseline JIT does not run the function's bytecode liveness analysis to size its value-profile buffer; the buffers of the catches that have executed are created when the function first crosses its DFG threshold (that tier-up is delayed once so they can profile), and op_catch profiles only once its buffer exists."_s) \
    v(Bool, useBaselineJITCodeSharing, jitEnabledByDefault(), Normal, nullptr) \
    v(Bool, libpasScavengeContinuously, false, Normal, nullptr) \
    v(Unsigned, libpasForcePGMWithRate, 0, Normal, "Forces on probablistic guard malloc and guards allocations with a rate 1/N (0 is disabled)"_s) \
    v(Bool, useWasmFaultSignalHandler, true, Normal, nullptr) \
    v(Bool, dumpUnlinkedDFGValidation, false, Normal, nullptr) \
    v(Bool, dumpWasmOpcodeStatistics, false, Normal, nullptr) \
    v(Bool, dumpWasmWarnings, false, Normal, nullptr) \
    v(Bool, useRecursiveJSONParse, true, Normal, nullptr) \
    v(Unsigned, thresholdForStringReplaceCache, 0x1000, Normal, nullptr) \
    v(Bool, useWasmIPInt, ipintEnabledByDefault(), Normal, "Use the in-place interpereter for WASM instead of LLInt."_s) \
    v(Bool, useWasmIPIntPrologueOSR, true, Normal, "Allow IPInt to tier up during function prologues"_s) \
    v(Bool, useWasmIPIntLoopOSR, true, Normal, "Allow IPInt to tier up during loop iterations"_s) \
    v(Bool, useWasmIPIntEpilogueOSR, true, Normal, "Allow IPInt to tier up during function epilogues"_s) \
    v(Bool, useWasmIPIntSIMD, true, Normal, "Allow IPInt to interpret SIMD code"_s) \
    v(Bool, traceWasmIPIntExecution, false, Normal, nullptr) \
    v(Bool, forceAllFunctionsToUseSIMD, false, Normal, "Force all functions to act conservatively w.r.t fp/vector registers for testing."_s) \
    v(Bool, useOMGInlining, true, Normal, "Use OMG inlining"_s) \
    v(Bool, freeRetiredWasmCode, true, Normal, "free BBQ/OMG-OSR wasm code once it's no longer reachable."_s) \
    v(Bool, useArrayAllocationSinking, true, Normal, nullptr) \
    v(Bool, dumpFTLCodeSize, false, Normal, nullptr) \
    v(Bool, dumpOptimizationTracing, false, Normal, nullptr) \
    v(Bool, dumpIonGraph, false, Normal, nullptr) \
    v(OptionString, ionGraphDirectory, nullptr, Normal, "Directory to place IonGraph"_s) \
    v(Unsigned, markedBlockDumpInfoCount, 0, Normal, nullptr) /* FIXME: rdar://139998916 */ \
    \
    /* Feature Flags */\
    \
    /* Feature-flag options whose source of truth is UnifiedWebPreferences.yaml. */ \
    FOR_EACH_JSC_WEB_PREFERENCE_OPTION(v) \
    /* Restricted so some app doesn't set this environment variable and start using it. */ \
    v(Bool, disallowMixedWasmExceptions, true, Restricted, "Disallow using both legacy and modern (try_table) wasm exception specs in the same module."_s) \
    /* Not sourced from UnifiedWebPreferences.yaml: force-enabled via the cross-origin-isolation path and consumed in WebCore. */ \
    v(Bool, useSharedArrayBuffer, false, Normal, nullptr) \
    /* Not sourced from UnifiedWebPreferences.yaml: shares its semantics with the WebCore-bound TrustedTypes feature. */ \
    v(Bool, useTrustedTypes, true, Normal, "Enable trusted types eval protection feature."_s) \



enum OptionEquivalence {
    SameOption,
    InvertedOption,
};

#define FOR_EACH_JSC_ALIASED_OPTION(v) \
    v(enableFunctionDotArguments, useFunctionDotArguments, SameOption) \
    v(enableTailCalls, useTailCalls, SameOption) \
    v(showDisassembly, dumpDisassembly, SameOption) \
    v(showDFGDisassembly, dumpDFGDisassembly, SameOption) \
    v(showFTLDisassembly, dumpFTLDisassembly, SameOption) \
    v(dumpGraphAtEachDFGFTLPhase, dumpDFGFTLGraphAtEachPhase, SameOption) \
    v(dumpGraphAtEachDFGPhase, dumpDFGGraphAtEachPhase, SameOption) \
    v(dumpGraphAtEachB3Phase, dumpB3GraphAtEachPhase, SameOption) \
    v(dumpGraphAtEachAirPhase, dumpAirGraphAtEachPhase, SameOption) \
    v(alwaysDoFullCollection, useGenerationalGC, InvertedOption) \
    v(enableOSREntryToDFG, useOSREntryToDFG, SameOption) \
    v(enableOSREntryToFTL, useOSREntryToFTL, SameOption) \
    v(enableAccessInlining, useAccessInlining, SameOption) \
    v(enablePolyvariantDevirtualization, usePolyvariantDevirtualization, SameOption) \
    v(enablePolymorphicAccessInlining, usePolymorphicAccessInlining, SameOption) \
    v(enablePolymorphicCallInlining, usePolymorphicCallInlining, SameOption) \
    v(enableObjectAllocationSinking, useObjectAllocationSinking, SameOption) \
    v(enableConcurrentJIT, useConcurrentJIT, SameOption) \
    v(enableProfiler, useProfiler, SameOption) \
    v(enableArchitectureSpecificOptimizations, useArchitectureSpecificOptimizations, SameOption) \
    v(objectsAreImmortal, useImmortalObjects, SameOption) \
    v(disableGC, useGC, InvertedOption) \
    v(enableTypeProfiler, useTypeProfiler, SameOption) \
    v(enableControlFlowProfiler, useControlFlowProfiler, SameOption) \
    v(enableExceptionFuzz, useExceptionFuzz, SameOption) \
    v(enableExecutableAllocationFuzz, useExecutableAllocationFuzz, SameOption) \
    v(enableOSRExitFuzz, useOSRExitFuzz, SameOption) \
    v(enableDollarVM, useDollarVM, SameOption) \
    v(maximumOptimizationCandidateInstructionCount, maximumOptimizationCandidateBytecodeCost, SameOption) \
    v(maximumFTLCandidateInstructionCount, maximumFTLCandidateBytecodeCost, SameOption) \
    v(maximumInliningCallerSize, maximumInliningCallerBytecodeCost, SameOption) \
    v(validateBCE, validateBoundsCheckElimination, SameOption) \


enum ExperimentalOptionFlags {
    LLIntAndBaselineOnly = 0,
    SupportsDFG = 1 << 0,
    SupportsFTL = 1 << 1,
};

#define FOR_EACH_JSC_EXPERIMENTAL_OPTION(v) \

constexpr size_t countNumberOfJSCOptions()
{
#define COUNT_OPTION(type_, name_, defaultValue_, availability_, description_) count++;
    size_t count = 0;
    FOR_EACH_JSC_OPTION(COUNT_OPTION);
    return count;
#undef COUNT_OPTION
}

constexpr size_t NumberOfOptions = countNumberOfJSCOptions();

class OptionRange {
private:
    enum RangeState { Uninitialized, InitError, Normal, Inverted };
public:
    OptionRange() = default;
    OptionRange(std::nullptr_t) { }

    bool init(const char*);
    bool NODELETE isInRange(unsigned) const;
    const char* rangeString() const { return (m_state > InitError) ? m_rangeString : s_nullRangeStr; }

    void dump(PrintStream& out) const;

private:
    JS_EXPORT_PRIVATE static const char* const s_nullRangeStr;

    RangeState m_state { Uninitialized };
    const char* m_rangeString { nullptr };
    unsigned m_lowLimit { 0 };
    unsigned m_highLimit { 0 };
};

enum class OSLogType : uint8_t {
    None,
    // These corresponds to OS_LOG_TYPE_xxx.
    Default,
    Info,
    Debug,
    Error,
    Fault
};

struct OptionsStorage {
    using Bool = bool;
    using Unsigned = unsigned;
    using Double = double;
    using Int32 = int32_t;
    using Size = size_t;
    using OptionRange = JSC::OptionRange;
    using OptionString = const char*;
    using GCLogLevel = GCLogging::Level;
    using OSLogType = JSC::OSLogType;

    bool allowUnfinalizedAccess;
    bool isFinalized;

#define DECLARE_OPTION(type_, name_, defaultValue_, availability_, description_) \
    type_ name_;
FOR_EACH_JSC_OPTION(DECLARE_OPTION)
#undef DECLARE_OPTION
};

// Options::Metadata's offsetOfOption and offsetOfOptionDefault relies on this.
static_assert(sizeof(OptionsStorage) <= 16 * KB);

} // namespace JSC
