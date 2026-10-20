#!/usr/bin/env python3
"""Checks the rules for ahead-of-time compiler code that source text can show. Takes a few seconds.

    check-code-rules.py <JavaScriptCore directory> [--advice] [--rule <letter>]..
    check-code-rules.py --self-test

Prints one line per finding, <path>:<line>: <rule letter>: <sentence>, sorted by path and line, then "<n> files, <m> findings".
Paths are relative to the JavaScriptCore directory. Exit code: 1 if an error rule has a finding, 2 for a bad command line, else 0.

    L  layout         aot/ holds AOT.cmake, Sources.txt and the folders of FOLDERS
    N  names          a header starts with AOT, no name has Typed, the base name of an included file is unique in JavaScriptCore
    I  includes       an AoT header is included by its base name
    E  feature guard  every .h and .cpp has ENABLE(AOT)
    Z  file size      1,000 lines and 80,000 bytes
    F  function size  100 lines (advice)
    G  environment    no getenv outside aot/tests/
    O  options        only the options of RUNTIME_OPTIONS, each only in its files
    T  mutable state  no mutable static or thread-local variable in compiler code
    V  validation     only VALIDATION_READERS ask whether a build validates
    W  words          no file name uses a discouraged word

--advice also prints the advice rules and every listed exception that applied, with its reason. Neither changes the exit code.

Text is read line by line with comments removed and string literals blanked. G, O, T and V read .h, .cpp and .inc files.
A .def file is data, often in a grammar of its own, and a .ts file is TypeScript: only L, N, Z and W apply to them.
A generated file follows its generator's rules on feature guards and sizes: E, Z and F skip GENERATED_FOLDERS.

The tables below are the only configuration:

    FOLDERS                     the folders of aot/
    NESTED_FOLDERS              the folders inside them. aot/tests/ may hold any
    GENERATED_FOLDERS           folders that a generator writes
    COMPILE_FOLDERS             folders that hold build state only
    MIXED_FOLDER_RUNTIME_FILES  the run-time files of builtins/ and image/, which hold both halves. A header goes with its .cpp
    RUNTIME_OPTIONS             option name -> globs of the files that may read it. aot/tests/ may read and set any option
    OPTION_EXCEPTIONS           (path, option name) -> reason
    STATE_EXCEPTIONS            (path, identifier) -> reason
    VALIDATION_READERS          globs of the files that may ask whether a build validates
    UNGUARDED_FILES             path -> reason
"""
import argparse
import collections
import contextlib
import fnmatch
import functools
import io
import os
import posixpath
import re
import sys
import tempfile

FOLDERS = ('analysis', 'builtins', 'codegen', 'flow', 'image', 'ir', 'object', 'passes', 'pipeline', 'place', 'represent', 'runtime', 'select', 'stubs', 'support', 'tests')
COMPILE_FOLDERS = ('analysis', 'codegen', 'flow', 'ir', 'passes', 'pipeline', 'place', 'represent', 'select', 'stubs', 'support')
MIXED_FOLDERS = ('builtins', 'image')
MIXED_FOLDER_RUNTIME_FILES = (
    'aot/builtins/*Operations.cpp',
    'aot/builtins/*Runtime.cpp',
    'aot/builtins/AOTBuiltinTableRealm.cpp',
    'aot/builtins/AOTOwnProperty*.cpp',
    'aot/image/AOTImage.cpp',
    'aot/image/AOTImageLoading.cpp',
    'aot/image/AOTFaultHandler.cpp',
    'aot/image/AOTRegExpInProcessLoader.cpp',
)

RUNTIME_FOLDERS = ('aot/runtime/*', 'aot/object/*', 'aot/image/*', 'aot/builtins/*')
LOGGING_OPTIONS_READER = ('aot/support/AOTCompileOptions.cpp',)
SHELL = ('aot/tests/AOTShell.cpp',)
RUNTIME_OPTIONS = {
    'useAOT': RUNTIME_FOLDERS,
    'markedBlockCageSizeInMB': RUNTIME_FOLDERS,
    'useAOTPerfMap': RUNTIME_FOLDERS,
    'aotCountsPath': RUNTIME_FOLDERS,
    'dumpAOTObjectCensusAtExit': RUNTIME_FOLDERS,
    'crashIfInterpreterIsUsed': RUNTIME_FOLDERS,
    'dropLastFastVisitReferenceForTesting': RUNTIME_FOLDERS,
    'useEdenCollectionsAtSlowPathAllocs': RUNTIME_FOLDERS,
    'disallowNonStaticCode': RUNTIME_FOLDERS,
    'sizeClassProgressionWithAOT': RUNTIME_FOLDERS,
    'sizeClassProgressionForCompiledCode': ('aot/support/AOTCompileOptionWords.cpp',),
    'validateAOTRuntime': ('aot/runtime/AOTVMState.cpp',),
    'aotImagePath': SHELL,
    'logAOTPhaseTimes': LOGGING_OPTIONS_READER,
    'dumpAOTIR': LOGGING_OPTIONS_READER,
    'dumpAOTB3Graph': LOGGING_OPTIONS_READER,
    'dumpAOTDisassembly': LOGGING_OPTIONS_READER,
    'aotFunctionToDump': LOGGING_OPTIONS_READER,
    'aotMapFilePath': LOGGING_OPTIONS_READER,
}
OPTION_EXCEPTIONS = {
    ('aot/object/AOTFunctionTables.cpp', 'useJIT'): 'a run-time assertion that no JIT runs beside compiled code',
    ('aot/runtime/AOTCompactStorage.cpp', 'useJIT'): 'a run-time assertion: DFG and FTL code would read the storage of a compact table as a butterfly',
    ('aot/runtime/AOTCallSlowPaths.cpp', 'breakOnThrow'): 'an engine option: with it, a throw in compiled code goes through VM::throwException, which breaks',
    ('aot/runtime/AOTVMState.cpp', 'verifyGC'): 'an engine option: with it, the heap checks inline cell visits against the method table, as it does for an image that validates',
    ('aot/runtime/AOTStackTrace.cpp', 'useAsyncStackTrace'): 'an engine option: a stack trace has async frames when Interpreter::getStackTrace would add them',
}

STATE_EXCEPTIONS = {
    ('aot/builtins/AOTBuiltinTableBindings.cpp', 's_host'): 'the code addresses that the host registers: set once, read-only afterwards',
}

VALIDATION_READERS = (
    'aot/support/AOTCompileOptions.*',
    'aot/support/AOTCompileOptionWords.cpp',
    'aot/support/AOTBuildContext.*',
    'aot/pipeline/AOTPassRunner.cpp',
    'aot/codegen/AOTB3Policy.cpp',
    'aot/codegen/AOTCodeGenerator.cpp',
    'aot/codegen/AOTCodeGeneratorDispatch.cpp',
    'aot/codegen/AOTValidationEmitter.*',
    'aot/codegen/AOTValidationTestCommands.cpp',
    'aot/codegen/AOTRuntimeCallEmitter.cpp',
    'aot/codegen/AOTBarrierEmitter.cpp',
    'aot/stubs/*',
    'aot/image/AOTCompile.cpp',
    'aot/image/AOTCodeGenerationDriver.cpp',
    'aot/image/AOTRelocation.*',
    'aot/image/AOTMappedAddressValidation.cpp',
    'aot/runtime/AOTVMState.cpp',
    'aot/tests/*',
)

UNGUARDED_FILES = {
    'aot/runtime/AOTRemovableFeature.h': 'used in every build',
    'aot/runtime/AOTNoFeatureRemoved.cpp': 'used in every build',
    'aot/image/AOTImageLoading.h': 'used in every build: without AoT, useImage() answers Unsupported',
    'aot/image/AOTWasmInterface.h': 'used in every build that has WebAssembly: Wasm::CompilationContext points to a CodeForImage',
}

MAX_FILE_LINES = 1000
MAX_FILE_BYTES = 80000
MAX_FUNCTION_LINES = 100
SOURCE_SUFFIXES = ('.h', '.cpp', '.def', '.inc', '.ts')
CODE_SUFFIXES = ('.h', '.cpp', '.inc')
INCLUDED_SUFFIXES = ('.h', '.def', '.inc')
INCLUDING_SUFFIXES = ('.h', '.cpp', '.mm', '.inc')
NESTED_FOLDERS = ('aot/builtins/generated', 'aot/builtins/handwritten', 'aot/builtins/models', 'aot/ir/generated', 'aot/ir/schema')
GENERATED_FOLDERS = ('aot/builtins/generated', 'aot/ir/generated')
DISCOURAGED_WORDS = frozenset(
    word + suffix
    for word in ('door island road world fact given born made kept held heir twin foreign stranger mend cure bear lacks yields behind refuse '
                 'the its that what which whether where whose yet too').split()
    for suffix in ('', 's'))

COMMENT_OR_LITERAL = re.compile('|'.join((
    r'#[ \t]*include[ \t]*"[^"\n]*"',
    r'//[^\n]*',
    r'/\*.*?\*/',
    r'R"([^()\\\s]*)\(.*?\)\1"',
    r'"(?:[^"\\\n]|\\.)*"',
    r"'(?:[^'\\\n]|\\.[^'\n]*)'",
)), re.S)
INCLUDE = re.compile(r'\s*#\s*include\s*(["<])([^">]+)[">]')
ENVIRONMENT = re.compile(r'\b(?:getenv|secure_getenv|_dupenv_s|environ)\b')
OPTION = re.compile(r'\bOptions::(\w+)')
STATE_TOKEN = re.compile(r'\bthread_local\b|\bLazyNeverDestroyed\b|\bstd::once_flag\b|\bstd::call_once\b')
STATIC_VARIABLE = re.compile(r'\s*(?:inline\s+)?static\s+(?:inline\s+)?([^()=;{]*)[=;{]')
GLOBAL_VARIABLE = re.compile(r'(\w[^()=;{]*\b[gs]_\w+\s*(?:\[[^\]]*\]\s*)*)[=;{]')
VALIDATION_QUESTION = re.compile(r'\b(?:validates|verifiesAfterEveryPass|validatesBackEnd|validatesReferences|CheckSet::\w+)\b')
RUNTIME_VALIDATION_QUESTION = re.compile(r'\bvalidatesRuntime\b')
NAME_WORD = re.compile(r'[A-Z]+(?![a-z])|[A-Z]?[a-z]+')

Finding = collections.namedtuple('Finding', 'path line rule sentence is_advice', defaults=(False,))


def finding_or_exception(path, line, rule, sentence, reason):
    if reason:
        return Finding(path, line, rule, f'{sentence}. Listed exception: {reason}', True)
    return Finding(path, line, rule, sentence)


def matches(path, globs):
    return any(fnmatch.fnmatchcase(path, glob) for glob in globs)


def is_outside_tests(path):
    return not path.startswith('aot/tests/')


def blank(match):
    text = match.group()
    if text[0] == '#':
        return text
    return (' ' if text[0] == '/' else text[-1] * 2) + '\n' * text.count('\n')


class Tree:
    def __init__(self, root):
        self.root = root
        self.files = []
        for folder, subfolders, names in os.walk(root):
            subfolders[:] = [name for name in subfolders if not name.startswith('.')]
            relative = os.path.relpath(folder, root)
            prefix = '' if relative == '.' else relative.replace(os.sep, '/') + '/'
            self.files += [prefix + name for name in names if not name.startswith('.')]
        self.files.sort()
        self.aot_files = [path for path in self.files if path.startswith('aot/')]
        self.sources = [path for path in self.aot_files if path.endswith(SOURCE_SUFFIXES)]
        self.code_files = [path for path in self.aot_files if path.endswith(CODE_SUFFIXES)]
        self.examined = set(self.aot_files)

    def text(self, path):
        self.examined.add(path)
        with open(os.path.join(self.root, path), encoding='utf-8', errors='replace') as file:
            return file.read()

    @functools.lru_cache(maxsize=None)
    def lines(self, path):
        return COMMENT_OR_LITERAL.sub(blank, self.text(path)).split('\n')

    def code_lines(self, includes_file):
        for path in self.code_files:
            if includes_file(path):
                for number, line in enumerate(self.lines(path), 1):
                    yield path, number, line

    def includes(self, path):
        if path.startswith('aot/') or 'AOT' in self.text(path):
            for number, line in enumerate(self.lines(path), 1):
                match = INCLUDE.match(line)
                if match:
                    yield number, match.group(1), match.group(2)


def check_layout(tree):
    for path in tree.aot_files:
        folder = posixpath.dirname(path)
        top = '/'.join(folder.split('/')[:2])
        if folder == 'aot':
            if path not in ('aot/AOT.cmake', 'aot/Sources.txt'):
                yield Finding(path, 1, 'L', 'only AOT.cmake and Sources.txt belong directly in aot/')
        elif top[len('aot/'):] not in FOLDERS:
            yield Finding(top, 1, 'L', 'not a folder of the compiler')
        elif folder != top and top != 'aot/tests' and folder not in NESTED_FOLDERS:
            yield Finding(folder, 1, 'L', f'the only nested folders are {", ".join(NESTED_FOLDERS)} and those below aot/tests')


def check_names(tree):
    paths_by_name = collections.defaultdict(list)
    for path in tree.files:
        if path.endswith(INCLUDED_SUFFIXES):
            paths_by_name[posixpath.basename(path)].append(path)
    for path in tree.sources:
        name = posixpath.basename(path)
        if name.endswith('.h') and not name.startswith('AOT'):
            yield Finding(path, 1, 'N', 'header name must start with AOT')
        if re.search(r'Typed(?!Array)', name):
            yield Finding(path, 1, 'N', 'file name must not contain Typed, except in TypedArray')
        others = [other for other in paths_by_name.get(name, ()) if other != path]
        if others:
            yield Finding(path, 1, 'N', f'{", ".join(others)} has the same base name, and all headers share one include path')


def check_includes(tree):
    for path in tree.files:
        if path.endswith(INCLUDING_SUFFIXES):
            for number, quote, included in tree.includes(path):
                name = posixpath.basename(included)
                if quote == '"' and name != included and name.startswith('AOT'):
                    yield Finding(path, number, 'I', f'include "{name}" by its base name, not as "{included}"')


def is_generated(path):
    return posixpath.dirname(path) in GENERATED_FOLDERS


def check_feature_guard(tree):
    for path in tree.aot_files:
        if is_generated(path):
            continue
        if path.endswith(('.h', '.cpp')) and not any('ENABLE(AOT)' in line for line in tree.lines(path)):
            yield finding_or_exception(path, 1, 'E', 'no ENABLE(AOT)', UNGUARDED_FILES.get(path))


def check_file_size(tree):
    for path in tree.sources:
        if is_generated(path):
            continue
        lines = tree.lines(path)
        count = len(lines) - (not lines[-1])
        if count > MAX_FILE_LINES:
            yield Finding(path, MAX_FILE_LINES + 1, 'Z', f'{count:,} lines, and the limit is {MAX_FILE_LINES:,}')
        size = len(tree.text(path).encode('utf-8'))
        if size > MAX_FILE_BYTES:
            yield Finding(path, 1, 'Z', f'{size:,} bytes, and the limit is {MAX_FILE_BYTES:,}')


def check_function_size(tree):
    for path in tree.code_files:
        if not path.endswith('.cpp') or is_generated(path):
            continue
        opening = None
        for number, line in enumerate(tree.lines(path), 1):
            if line == '{':
                opening = number
            elif line == '}' and opening:
                count = number - opening - 1
                if count > MAX_FUNCTION_LINES:
                    yield Finding(path, opening, 'F', f'a function body of {count:,} lines, and the limit is {MAX_FUNCTION_LINES}', True)
                opening = None


def check_environment(tree):
    for path, number, line in tree.code_lines(is_outside_tests):
        match = ENVIRONMENT.search(line)
        if match:
            yield Finding(path, number, 'G', f'{match.group()}: AoT code does not read the environment')


def check_options(tree):
    for path, number, line in tree.code_lines(is_outside_tests):
        for name in OPTION.findall(line):
            readers = RUNTIME_OPTIONS.get(name)
            if readers is None:
                sentence = f'{name} is not an option AoT code may read'
            elif matches(path, readers):
                continue
            else:
                sentence = f'{name} may be read only in {", ".join(readers)}'
            yield finding_or_exception(path, number, 'O', sentence, OPTION_EXCEPTIONS.get((path, name)))


def is_compiler_file(path):
    folder = path.split('/')[1]
    return folder in COMPILE_FOLDERS or (folder in MIXED_FOLDERS and not matches(posixpath.splitext(path)[0] + '.cpp', MIXED_FOLDER_RUNTIME_FILES))


def without_template_arguments(text):
    while True:
        shorter = re.sub(r'<[^<>]*>', '', text)
        if shorter == text:
            return text
        text = shorter


def is_immutable(declaration):
    declaration = without_template_arguments(declaration)
    last_const_end = max((match.end() for match in re.finditer(r'\bconst\b', declaration)), default=-1)
    return 'constexpr' in declaration or last_const_end > declaration.rfind('*')


def declared_name(line):
    call = re.search(r'\bcall_once\s*\(\s*(\w+)', line)
    if call:
        return call.group(1)
    head = re.split(r'[=;{(]', without_template_arguments(line), maxsplit=1)[0]
    words = re.findall(r'\w+', re.sub(r'\[[^\]]*\]', '', head))
    return words[-1] if words else STATE_TOKEN.search(line).group()


def check_mutable_state(tree):
    for path, number, line in tree.code_lines(is_compiler_file):
        if not STATE_TOKEN.search(line):
            match = STATIC_VARIABLE.match(line) or GLOBAL_VARIABLE.match(line)
            if not match or is_immutable(match.group(1)) or 'operator' in match.group(1):
                continue
        name = declared_name(line)
        yield finding_or_exception(path, number, 'T', f'{name} is mutable static state, and build state belongs in BuildContext',
                                   STATE_EXCEPTIONS.get((path, name)))


def check_validation_readers(tree):
    for path in tree.code_files:
        questions = []
        if not matches(path, VALIDATION_READERS):
            questions.append((VALIDATION_QUESTION, 'only VALIDATION_READERS ask whether a build validates'))
        if path.split('/')[1] in COMPILE_FOLDERS:
            questions.append((RUNTIME_VALIDATION_QUESTION, 'a question for the run time, never for the compiler'))
        if not questions:
            continue
        for number, line in enumerate(tree.lines(path), 1):
            for question, sentence in questions:
                match = question.search(line)
                if match:
                    yield Finding(path, number, 'V', f'{match.group()}: {sentence}')


def check_words(tree):
    for path in tree.aot_files:
        words = [word for word in NAME_WORD.findall(posixpath.basename(path)) if word.lower() in DISCOURAGED_WORDS]
        if words:
            yield Finding(path, 1, 'W', f'the file name uses {", ".join(words)}')


RULES = {
    'L': check_layout,
    'N': check_names,
    'I': check_includes,
    'E': check_feature_guard,
    'Z': check_file_size,
    'F': check_function_size,
    'G': check_environment,
    'O': check_options,
    'T': check_mutable_state,
    'V': check_validation_readers,
    'W': check_words,
}

GUARD = '#if ENABLE(AOT)\n'
SELF_TEST_TREE = (
    ('CMakeLists.txt', '', ''),
    ('aot/AOT.cmake', '', ''),
    ('aot/Sources.txt', '', ''),
    ('aot/AOTStray.h', GUARD, 'L'),
    ('aot/extra', None, 'L'),
    ('aot/extra/AOTExtra.h', GUARD, ''),
    ('aot/passes/deep', None, 'L'),
    ('aot/passes/deep/AOTDeep.h', GUARD, ''),
    ('aot/builtins/handwritten/MapPrototype.ts', '', ''),
    ('aot/ir/schema/operations.def', '# Options::useJIT, getenv\nstatic Math.PI = number\n', ''),
    ('aot/tests/passes/sink-allocations/escapes-through-call.ir', '', ''),
    ('aot/ir/Value.h', GUARD, 'N'),
    ('aot/ir/AOTTypedValue.h', GUARD, 'N'),
    ('aot/codegen/AOTTypedArrayEmitter.h', GUARD, ''),
    ('aot/place/AOTTables.h', GUARD, 'N'),
    ('aot/runtime/AOTTables.h', GUARD, 'N'),
    ('runtime/JSObject.cpp', '#include "aot/runtime/AOTVMState.h"\n#include "AOTVMState.h"\n// #include "aot/AOTOld.h"\n', 'I'),
    ('aot/ir/AOTUnguarded.h', '// ENABLE(AOT)\n', 'E'),
    ('aot/ir/generated/AOTSchemaOperations.cpp', 'void run()\n{\n' + '\n' * MAX_FILE_LINES + '}\n', ''),
    ('aot/runtime/AOTNoFeatureRemoved.cpp', '', 'e'),
    ('aot/ir/AOTLong.h', GUARD + '\n' * MAX_FILE_LINES, 'Z'),
    ('aot/ir/AOTFull.h', GUARD + '\n' * (MAX_FILE_LINES - 1), ''),
    ('aot/ir/AOTWide.h', GUARD + 'x' * MAX_FILE_BYTES + '\n', 'Z'),
    ('aot/ir/AOTNarrow.h', GUARD + 'x' * (MAX_FILE_BYTES - len(GUARD) - 1) + '\n', ''),
    ('aot/builtins/Array.builtins.ts', '\n' * (MAX_FILE_LINES + 1), 'Z'),
    ('aot/ir/AOTLongFunction.cpp', GUARD + 'void run()\n{\n' + '\n' * (MAX_FUNCTION_LINES + 1) + '}\n', 'f'),
    ('aot/ir/AOTFullFunction.cpp', GUARD + 'void run()\n{\n' + '\n' * MAX_FUNCTION_LINES + '}\n', ''),
    ('aot/runtime/AOTReadsEnvironment.cpp', GUARD + 'const char* path = getenv("PATH");\n', 'G'),
    ('aot/passes/AOTReadsOptions.cpp', GUARD + 'bool a = Options::useAOT();\nbool b = Options::useDFGJIT();\nauto c = CompileOptions::fromWire(wire);\n', 'OO'),
    ('aot/place/AOTStructureDescriptorTable.cpp', GUARD + 'auto progression = Options::sizeClassProgressionWithAOT();\n', 'O'),
    ('aot/support/AOTBuildContext.cpp', GUARD + 'options.sizeClassProgression = Options::sizeClassProgressionWithAOT();\n', 'O'),
    ('aot/support/AOTCompileOptionWords.cpp', GUARD + 'options.sizeClassProgression = Options::sizeClassProgressionForCompiledCode;\n', ''),
    ('aot/object/AOTFunctionTables.cpp', GUARD + 'RELEASE_ASSERT(!Options::useJIT());\n', 'o'),
    ('aot/runtime/AOTVMState.cpp', GUARD + 'static Lock s_lock;\nbool on = options.validates || Options::validateAOTRuntime() || Options::useAOT();\n', ''),
    ('aot/passes/AOTKeepsState.cpp', GUARD + 'static unsigned counter;\nstatic thread_local unsigned t_depth;\nLock g_lock;\nstatic const Table* s_table;\n'
     '    static NeverDestroyed<Vector<int>> list;\n    std::call_once(flag, [] { });\n{ thread_local int depth = 0; use(depth); }\n', 'TTTTTTT'),
    ('aot/passes/AOTKeepsNoState.cpp', GUARD + 'static constexpr unsigned sizes[] = { 1, 2 };\nstatic const bool isEnabled = true;\n'
     'static void* const sentinel = nullptr;\nstatic bool isGood(int value)\n{\n    return static_cast<bool>(value);\n}\nstatic_assert(sizeof(int) == 4);\n'
     '// static unsigned counter; getenv\n/* thread_local\n   Options::useJIT */\nconst char* text = "validates() environ";\n'
     'bool changed = analyses.invalidates(pass);\n', ''),
    ('aot/represent/AOTChooseRepresentations.cpp', GUARD + 'static unsigned s_count;\n', 'T'),
    ('aot/builtins/AOTStringBuiltinEmitter.cpp', GUARD + 'static unsigned s_calls;\n', 'T'),
    ('aot/builtins/AOTStringBuiltinOperations.cpp', GUARD + 'static unsigned s_calls;\n', ''),
    ('aot/builtins/AOTBuiltinTableBindings.cpp', GUARD + 'static constinit std::atomic<const BuiltinBindingSet*> s_host { nullptr };\n', 't'),
    ('aot/support/AOTThreads.cpp', GUARD + 'static Lock s_parallelismLimitLock;\n', 'T'),
    ('aot/passes/AOTAsksValidation.cpp', GUARD + 'bool a = build.validates();\nbool b = validatesRuntime(vm);\nbool c = hasFlag(checks, CheckSet::Validate);\n', 'VVV'),
    ('aot/runtime/AOTRuntimeValidation.h', GUARD + 'bool validatesRuntime(VM&);\n', ''),
    ('aot/passes/AOTPassTestCommands.cpp', GUARD, ''),
    ('aot/tests/testaot.cpp', GUARD + 'Options::useJIT() = false;\nbool on = options.validates;\nconst char* request = getenv("REQUEST");\n', ''),
    ('aot/tests/AOTShell.cpp', GUARD, ''),
    ('aot/tests/flow/the-given-rows.ir', '', 'W'),
    ('aot/passes/AOTWorldFacts.h', GUARD, 'W'),
    ('aot/passes/AOTThreadJumps.h', GUARD, ''),
)


def self_test():
    def run(*arguments):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            exit_code = main(list(arguments))
        return exit_code, output.getvalue()

    problems = []
    with tempfile.TemporaryDirectory() as root:
        def write(path, text):
            os.makedirs(os.path.dirname(os.path.join(root, path)), exist_ok=True)
            with open(os.path.join(root, path), 'w', encoding='utf-8') as file:
                file.write(text)

        for path, text, _ in SELF_TEST_TREE:
            if text is not None:
                write(path, text)
        for flags, is_expected in (((), str.isupper), (('--advice',), str.isalpha)):
            expected = collections.Counter((path, letter.upper()) for path, _, letters in SELF_TEST_TREE for letter in letters if is_expected(letter))
            exit_code, output = run(root, *flags)
            found = collections.Counter(re.findall(r'^(.+?):\d+: (\w): ', output, re.M))
            label = ' '.join(flags) or 'no flags'
            problems += [f'{label}: {path} has no finding of rule {letter}' for path, letter in (expected - found).elements()]
            problems += [f'{label}: {path} has an unexpected finding of rule {letter}' for path, letter in (found - expected).elements()]
            if exit_code != 1:
                problems.append(f'{label}: exit code {exit_code}, expected 1')
        if run(root, '--advice', '--rule', 'F')[0]:
            problems.append('an advice rule changed the exit code')
        try:
            with contextlib.redirect_stderr(io.StringIO()):
                run(os.path.join(root, 'aot'))
            problems.append('a directory without CMakeLists.txt was accepted')
        except SystemExit as error:
            if error.code != 2:
                problems.append(f'bad directory: exit code {error.code}, expected 2')
    for problem in problems:
        print(problem)
    print('self test failed' if problems else 'self test passed')
    return 1 if problems else 0


def main(arguments=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('directory', nargs='?')
    parser.add_argument('--advice', action='store_true')
    parser.add_argument('--rule', action='append', choices=tuple(RULES), metavar='<letter>')
    parser.add_argument('--self-test', action='store_true')
    options = parser.parse_args(arguments)

    if options.self_test:
        exit_code = self_test()
        if exit_code or not options.directory:
            return exit_code
    root = options.directory
    if not root or not os.path.isfile(os.path.join(root, 'CMakeLists.txt')) or not os.path.isdir(os.path.join(root, 'aot')):
        parser.error('expected a JavaScriptCore directory, with CMakeLists.txt and aot/')
    tree = Tree(root)
    findings = sorted({finding for letter in options.rule or RULES for finding in RULES[letter](tree)})
    shown = [finding for finding in findings if options.advice or not finding.is_advice]
    for finding in shown:
        print(f'{finding.path}:{finding.line}: {finding.rule}: {finding.sentence}')
    print(f'{len(tree.examined)} files, {len(shown)} findings')
    return 1 if any(not finding.is_advice for finding in shown) else 0


if __name__ == '__main__':
    sys.exit(main())
