#!/usr/bin/env python3
# forced-jumps.py --javascriptcore <Source/JavaScriptCore> --out <folder>
#
# An operation that begins in its own frame leaves by no forced jump. Every build asks it of the text of the sources and headers of aot/: in one function, a line that begins an operation
# in the function's own frame, and behind it a line that forces a jump. A #define with the lines that continue it is read as a function of its own, under the macro's name.
# A function whose jump stands on a road that has begun nothing is named in aot/runtime/AOTForcedJumps.txt.
# It reads the fixtures beside it first, and fails where it does not find what they hold.
import argparse, os, re, sys

LIST = 'aot/runtime/AOTForcedJumps.txt'
FIXTURES = 'aot/runtime/generator/forced-jumps-fixtures'
STAMP = 'AOTForcedJumpsChecked.h'
FOLDERS_NOT_READ = ('node_modules', 'tests')
FOUND_IN_FIXTURES = (
    ('begins-in-a-block-and-leaves-by-a-forced-jump.txt', 'runtime/AOTMadeUp.cpp', ['operationReadOffBaseOfHome']),
    ('begins-in-a-block-and-leaves-by-a-forced-jump.txt', 'runtime/AOTMadeUp.h', ['operationReadOffBaseOfHome']),
    ('begins-in-a-block-and-leaves-by-a-forced-jump.txt', 'image/AOTMadeUp.cpp', ['operationReadOffBaseOfHome']),
    ('begins-in-a-block-and-leaves-by-a-forced-jump.txt', 'AOTMadeUp.cpp', ['operationReadOffBaseOfHome']),
    ('begins-in-a-block-and-leaves-by-a-forced-jump.txt', 'tests/AOTMadeUp.cpp', []),
    ('begins-in-a-block-and-leaves-by-a-forced-jump.txt', 'runtime/AOTMadeUp.txt', []),
    ('jumps-on-another-road-than-the-one-that-begins.txt', 'runtime/AOTMadeUp.cpp', ['joinShortParts']),
    ('forces-the-jump-by-an-attribute.txt', 'runtime/AOTMadeUp.cpp', ['operationReadOffBaseOfHome']),
    ('takes-the-record-of-its-frame-by-hand.txt', 'runtime/AOTMadeUp.cpp', ['operationThatAsksTheCompiler', 'operationThatNamesTheRecord']),
    ('a-macro-defines-the-operation.txt', 'runtime/AOTMadeUp.cpp', ['DEFINE_READ_OFF_BASE_OF_HOME']),
    ('leaves-by-no-forced-jump-behind-a-beginning.txt', 'runtime/AOTMadeUp.cpp', []),
)
begins = re.compile(r'\bAOT_OPERATION_BEGIN\w*\s*\(|\bAOT_OPERATION_FIND_OWN_FRAME\s*\(|\bAOT_OPERATION_FRAME_RECORD\w*\s*\(|\w*CallFrameTracer\b|\bAOT_FRAME_RECORD_OF_THIS_FUNCTION\b|\b__builtin_frame_address\b')
jumps = re.compile(r'\bmusttail\b|\b(?:AOT_)?MUST_TAIL_CALL\b')
defines = re.compile(r'\s*#\s*define\s+(\w+)')
defined = re.compile(r'^JSC_DEFINE\w*\((\w+)')
called = re.compile(r'([\w:~]+)\s*\(')


def is_read(file):
    return file.endswith(('.cpp', '.h')) and not any(folder in FOLDERS_NOT_READ for folder in file.split('/')[:-1])


def find(text, file):
    found = {}
    if not is_read(file) or not jumps.search(text):
        return found
    name, beginning, around = '?', 0, None
    for number, line in enumerate(text.split('\n'), 1):
        if around is None:
            macro = defines.match(line)
            if macro:
                around, name, beginning, line = (name, beginning), macro.group(1), 0, line[macro.end():]
            elif line.startswith('}'):
                name, beginning = '?', 0
                continue
            elif line.lstrip().startswith(('//', '#')):
                continue
            elif line[:1] not in ('', ' ', '\t', '{', ',', ':', '/') and not beginning:
                head = defined.match(line) or called.search(line)
                if head:
                    name = head.group(1)
                    continue
        if begins.search(line) and not beginning:
            beginning = number
        elif jumps.search(line) and beginning:
            found.setdefault((file, name), (beginning, number))
        if around is not None and not line.endswith('\\'):
            (name, beginning), around = around, None
    return found


def read_list(text):
    listed, problems = {}, []
    for line in text.split('\n'):
        if not line or line.startswith('#'):
            continue
        parts = line.split(' | ')
        if len(parts) != 3 or not all(part.strip() for part in parts):
            problems.append(f'{LIST}: not "<file> | <function> | <why its jump is right as it is>": {line}')
        else:
            listed[(parts[0], parts[1])] = parts[2]
    return listed, problems


def compare(found, listed):
    problems = []
    for (file, name), (beginning, jump) in sorted(found.items()):
        if (file, name) not in listed:
            problems.append(f'aot/{file}:{beginning}: {name}() begins in its own frame, and aot/{file}:{jump} forces a jump behind it. An operation that begins in its own frame leaves by no forced '
                            f'jump: let it call, or let both call one plain function. Where the jump stands on a road that has begun nothing, add a line to {LIST}: {file} | {name} | <why>')
    for file, name in sorted(listed):
        if (file, name) not in found:
            problems.append(f'{LIST}: {name}() of aot/{file} has no forced jump behind a beginning: take its line out.')
    return problems


def has_teeth(folder):
    for fixture, file, names in FOUND_IN_FIXTURES:
        found = find(open(os.path.join(folder, fixture), encoding='utf-8').read(), file)
        listed = {key: 'a reason' for key in found}
        if sorted(name for _, name in found) != names or len(compare(found, {})) != len(names) or compare(found, listed) or len(compare({}, listed)) != len(names):
            return False
    return True


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--javascriptcore', required=True)
    parser.add_argument('--out', required=True)
    arguments = parser.parse_args()
    root = os.path.abspath(arguments.javascriptcore)
    if not has_teeth(os.path.join(root, FIXTURES)):
        sys.exit(f'aot/runtime/generator/forced-jumps.py does not find in {FIXTURES} what stands there: it checks nothing.')
    found = {}
    for directory, folders, files in os.walk(os.path.join(root, 'aot')):
        folders[:] = sorted(folder for folder in folders if folder not in FOLDERS_NOT_READ)
        for name in sorted(files):
            file = os.path.relpath(os.path.join(directory, name), os.path.join(root, 'aot')).replace(os.sep, '/')
            if is_read(file):
                found.update(find(open(os.path.join(directory, name), encoding='utf-8').read(), file))
    listed, problems = read_list(open(os.path.join(root, LIST), encoding='utf-8').read())
    problems += compare(found, listed)
    if problems:
        sys.exit('\n'.join(problems))
    os.makedirs(arguments.out, exist_ok=True)
    open(os.path.join(arguments.out, STAMP), 'w').write('// Written by aot/runtime/generator/forced-jumps.py when the sources have passed it. Do not edit.\n')


main()
