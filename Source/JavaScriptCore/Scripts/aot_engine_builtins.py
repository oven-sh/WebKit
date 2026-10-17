"""The engine's builtins that are written in JavaScript, for the AOT compiler. Used by generate-aot-builtins.py.

builtins/*.js are written in a syntax of their own (`@name`). <bun>/src/js/builtins/*.ts are written in the same language with `$` in the place of `@`, which every
JavaScript parser reads. derive() rewrites the one into the other, token by token, outside of strings and comments:

  @name                      $name
  @undefined, @Infinity      undefined, Infinity            in such a file nothing can stand in their way
  @MAX_SAFE_INTEGER          9007199254740991
  @toNumber(x)               +(x)
  @toString(x)               `${x}`
  @throwTypeError(m);        throw new TypeError(m);        likewise @throwRangeError. Always a statement
  x.@name                    x.$name
  x.@@iterator               x[Symbol.iterator]
  { @@iterator: f }          { [Symbol.iterator]: f }
  @linkTimeConstant          $linkTimeConstant;             an annotation, which is a line of its own in front of a function, becomes a directive
  @visibility=Private        $visibility = "Private";
  @overriddenName="x"        $overriddenName = "x";
  function f(                export function f(
  function return(           $overriddenName = "return"; export function return_(
  delete x[y]                $deleteByValSloppy(x, y)       in a function without "use strict", where it yields false if it fails. The result is compiled as strict code
  @ifAbruptCloseIterator(iterator, expression);
                             what BytecodeIntrinsicNode::emit_intrinsic_ifAbruptCloseIterator() emits, written out: the expression under a `try`. It is the one intrinsic that
                             does not evaluate an argument before anything else, so it cannot be a function. If the expression throws, the iterator is closed, whatever that
                             throws is dropped, and the exception goes on. If it is a `yield` that is resumed by `return()`, the iterator is closed and what that throws goes on.

Nothing else changes, so a line of the result is the same line of the source.

@iteratorGenericNext() and @iteratorGenericClose() evaluate their arguments first: they are functions of <bun>/src/aot/prelude/engine/IteratorOperations.ts.

The number of a function is its BuiltinCodeIndex. functions() computes it as generate-js-builtins.py does, with that script's own parser, from the files CMakeLists.txt lists;
AOTTypedBuiltinsEngine.cpp asserts each."""
import os, re, sys

sys.path.append(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'wkbuiltins'))
from builtins_generator import BuiltinsGenerator
from builtins_model import BuiltinsCollection

TOKEN = re.compile(r'''(?P<skipped>//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"|'(?:\\.|[^'\\\n])*'|`(?:\\.|[^`\\])*`)|\.\s*@@(?P<member>\w+)|@@(?P<key>\w+)(?=\s*:)|@(?P<name>\w+)''', re.S)
SKIPPED = re.compile(r'''//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"|'(?:\\.|[^'\\\n])*'|`(?:\\.|[^`\\])*`''', re.S)
ANNOTATION = re.compile(r'^@(\w+)(?:=(.*))?$', re.M)
HEAD = re.compile(r'^(async\s+)?function\s+(\w+)\s*\(', re.M)
RESERVED = frozenset('''break case catch class const continue debugger default delete do else enum export extends false finally for function if import in instanceof new null return
super switch this throw true try typeof var void while with yield let static await'''.split())


def sources(javascriptcore):
    """The files generate-js-builtins.py is given, in that order."""
    cmake = open(os.path.join(javascriptcore, 'CMakeLists.txt')).read()
    listed = re.search(r'set\(JavaScriptCore_BUILTINS_SOURCES\n(.*?)\)', cmake, re.S).group(1)
    return [os.path.join(javascriptcore, path) for path in re.findall(r'\$\{JAVASCRIPTCORE_DIR\}/(\S+)', listed)]


def functions(javascriptcore):
    """(file without .js, function, name of the code, BuiltinCodeIndex) of every builtin."""
    collection = BuiltinsCollection('JavaScriptCore')
    for path in sources(javascriptcore):
        collection.parse_builtins_file(path, open(path).read())
    return [(function.object.object_name, function.function_name, BuiltinsGenerator.mangledNameForFunction(function) + 'Code', index) for index, function in enumerate(collection.all_functions())]


def argument_end(text, start):
    """Where the argument that starts there ends: at a `,` or `)` of its own level."""
    depth, position = 0, start
    while True:
        skipped = SKIPPED.match(text, position)
        if skipped:
            position = skipped.end()
            continue
        character = text[position]
        if character in ',)' and not depth:
            return position
        depth += (character in '([{') - (character in ')]}')
        position += 1


def expand_if_abrupt_close_iterator(text):
    call = '$ifAbruptCloseIterator('
    while call in text:
        start = text.index(call)
        comma = argument_end(text, start + len(call))
        end = argument_end(text, comma + 1)
        if text[comma] != ',' or text[end:end + 2] != ');' or text[text.rindex('\n', 0, start) + 1:start].strip():
            sys.exit('aot_engine_builtins.py: @ifAbruptCloseIterator(iterator, expression); is not a statement of its own: %s' % text[start:start + 80])
        text = (text[:start] + '{ let __iterator = ' + text[start + len(call):comma].strip() + ', __completion = 0; try { ' + text[comma + 1:end].lstrip(' ')
                + '; __completion = 1; } catch (__error) { __completion = 2; try { $iteratorGenericClose(__iterator); } catch { } throw __error; } finally { if (!__completion) $iteratorGenericClose(__iterator); } }'
                + text[end + 2:])
    return text


def expand_to_string(text):
    call = '$toString('
    while call in text:
        start = text.index(call)
        end = argument_end(text, start + len(call))
        if text[end] != ')':
            sys.exit('aot_engine_builtins.py: @toString(x) takes one argument: %s' % text[start:start + 80])
        text = text[:start] + '`${' + text[start + len(call):end] + '}`' + text[end + 1:]
    return text


DELETE = re.compile(SKIPPED.pattern + r'''|\bdelete\s+(?P<base>\w+)\[(?P<key>\w+)\]|(?P<other>\bdelete\b)''', re.S)


def rewrite_delete_in_sloppy_functions(text):
    def rewritten(match):
        if match.group('other'):
            sys.exit('aot_engine_builtins.py: a `delete` in a function without "use strict" that is not `delete x[y]`')
        return '$deleteByValSloppy(%s, %s)' % (match.group('base'), match.group('key')) if match.group('base') else match.group(0)

    for head in reversed(list(HEAD.finditer(text))):
        end = text.index('\n}\n', head.end())
        if '"use strict"' not in text[head.end():end]:
            text = text[:head.end()] + DELETE.sub(rewritten, text[head.end():end]) + text[end:]
    return text


def exported_name(name):
    return name + '_' if name in RESERVED else name


def derive(text, excluded_functions):
    """The text in the syntax of <bun>/src/js/builtins, without the functions named in `excluded_functions`, each of which goes with what is in front of it: annotations and comments."""
    heads = list(HEAD.finditer(text))
    for index in reversed(range(len(heads))):
        if heads[index].group(2) not in excluded_functions:
            continue
        end = text.index('\n}\n', heads[index].end()) + 3
        start = text.rindex('\n}\n', 0, heads[index].start()) + 3 if index else text.index('*/') + 3
        text = text[:start] + text[end:]
    text = rewrite_delete_in_sloppy_functions(text)

    marked = ANNOTATION.sub(lambda match: '\0%s%s;' % (match.group(1), ' = ' + (match.group(2) if match.group(2).startswith('"') else '"%s"' % match.group(2)) if match.group(2) else ''), text)

    def rewritten(match):
        if match.group('skipped'):
            return match.group(0)
        if match.group('member'):
            return '[Symbol.%s]' % match.group('member')
        if match.group('key'):
            return '[Symbol.%s]' % match.group('key')
        name = match.group('name')
        if name in ('undefined', 'Infinity'):
            return name
        if name == 'MAX_SAFE_INTEGER':
            return '9007199254740991'
        if name == 'toNumber':
            return '+'
        if name in ('throwTypeError', 'throwRangeError'):
            if marked[marked.rindex('\n', 0, match.start()) + 1:match.start()].strip():
                sys.exit('aot_engine_builtins.py: @%s(..) is not a statement of its own' % name)
            return 'throw new ' + name[5:]
        return '$' + name

    def head(match):
        name = match.group(2)
        directive = '$overriddenName = "%s"; ' % name if name in RESERVED else ''
        return '%sexport %sfunction %s(' % (directive, match.group(1) or '', exported_name(name))

    return expand_to_string(expand_if_abrupt_close_iterator(HEAD.sub(head, TOKEN.sub(rewritten, marked).replace('\0', '$'))))
