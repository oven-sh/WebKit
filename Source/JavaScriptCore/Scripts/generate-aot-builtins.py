#!/usr/bin/env python3
"""generate-aot-builtins.py [--bun <bun checkout>] [--check] [--list]

The AOT built-ins table. Its source of truth is aot/builtins/*.def, one file per family. This script writes the checked-in files that are
compiled:

  aot/AOTTypedBuiltinLists.h                     the X-macros (families, inline built-ins, language operations, run-time operations, prelude
                                                 functions, original structures), the sizes of the table, the numbers of the names, classes
                                                 and entries that C++ knows by name, and AOT_TYPED_BUILTINS_HASH
  aot/AOTTypedBuiltinEntries.inc                 the table, as constant arrays: entries, parameters with what the entry does with each operand, names,
                                                 classes, the index by (kind, holder, name); included by AOTTypedBuiltins.cpp
  aot/AOTTypedBuiltinOperations.inc              the declarations of the run-time operations, included by AOTTypedOperations.h
  aot/AOTTypedBuiltinMembers.inc                 includes the families' AOTTypedBuiltins<Family>Members.h in the body of class Lowering
  <bun>/src/sema/check/aot/builtins_generated.rs the names the checker trusts, and what each entry does with its operands
  <bun>/src/aot/builtins_generated.rs            the hash, the files and the functions of the prelude
  <bun>/src/jsc/bindings/AOTFrontEndBuiltinsHash.h the hash again, for a static_assert against the engine that Bun is compiled with
  <bun>/src/jsc/bindings/AOTHTTPHeaderNames.h    a static_assert for each `header` line against Bun's HTTPHeaderName
  <bun>/src/aot/prelude/engine/<File>.js         builtins/<File>.js in the syntax of <bun>/src/js/builtins: see aot_engine_builtins.py
  <bun>/src/aot/prelude/engine/functions.txt     the functions of that folder: BuiltinCodeIndex, file, function, separated by tabs
  <bun>/src/aot/build/intrinsics.rs              what each `$name` of Bun's src/js is
  <bun>/src/aot/build/intrinsics.txt             which names have a line, for src/codegen/builtins-manifest.ts: NAME or PREFIX*, a tab, POSITION;
                                                 then a tab and `hardened` if it is not only spelled `$NAME`

--check writes nothing and fails if a file is not what would be written. --list prints the table, one normalized line per entry.
The hash covers what the front end goes by (names, how many operands an operation takes, intrinsics, prelude functions, uses of operands). It is in the header of every program the front end
writes, and Program::tryRead() refuses a program with another hash.

A family is a file <family>.def. What is written in C++ for it is in files nobody else edits:
  aot/AOTTypedBuiltins<Family>.cpp               writesOfInline<Family>(), Lowering::lowerInline<Family>(), its run-time operations
  aot/AOTTypedBuiltins<Family>Members.h          optional: declarations of more members of class Lowering
  <bun>/src/aot/prelude/<family>.ts              optional: its functions of the prelude

One declaration per line. `#` starts a comment.

  class NAME : JSTYPE [= STRUCTURE {, STRUCTURE}]
      A class whose instances all have that JSType. STRUCTURE is a member call on JSGlobalObject that yields a structure the engine gives
      new instances. A method, getter or setter is only resolved on an object whose class lists its structures (those of Array, one for
      each indexing type, are in Instance::originalArrayStructureIDs).
  typedarray NAME
      NAMEArray: the class, listed under %TypedArray%, and its BYTES_PER_ELEMENT.
  replaceable NAME
      A global object that hosts and programs replace, as they do each of its properties. Compiled code reads both every time, and takes
      what an entry says to hold where the function it finds is the one the realm started with. Its entries are static functions:
      HostCall, Call, Generic or Inline(NAME).
  inline NAME [exact]
      InlineBuiltin::NAME, which this family lowers. Any family may use it. `exact`: the code does what the call by name does whatever has
      become of the receiver, so the receiver's original structure is not checked first.
  runtime NAME : WRITES RESULT (PARAMETERS) [noexcept [leaf]]
      operationAOTTypedNAME(Instance*, PARAMETERS), which this family defines and calls with AOT_TYPED_OPERATION(NAME, ..). `leaf`: without
      the Instance: it looks at its arguments and at nothing else. WRITES is a
      Typed::Writes. The types are those OperationSignature knows: EncodedJSValue, JSCell*, uint32_t, int32_t, uint64_t, double,
      const EncodedJSValue*; a result may also be void, size_t or void*.
  predicted PATH
      What lib.d.ts declares of HOLDER.NAME, or of the global NAME and all it leads to, says more than the function does (`Object.assign`
      is declared to return `T & U` and returns its first argument). The entries stay; the checker is not given the name, so that to
      it the declaration is one without a body like any other: a prediction. A holder that is left without a name is not given either.
  $NAME POSITION = WHAT
      What `$NAME` is in a file of Bun's src/js that does not declare it. POSITION: Call (`$NAME(..)`), Value (not called) or Member
      (`o.$NAME`, `o.$NAME(..)`, a key, a member of a class). `$PREFIX* POSITION = WHAT`: of every name that starts so and has no line.
      WHAT is a variant of `Intrinsic` in intrinsics.rs, which says what the front end emits for each:
      Operation(NAME) | HasJSType(FIRST, LAST) | Builtin(HOLDER.NAME) | Builtin(FUNCTION) | Construct(GLOBAL) | IsNullish | Undefined |
      Int(N, C++ EXPRESSION) | Bool(B) | Double(Infinity) | Expect(B) | Call | Apply | ArrayPush | PutByValDirect[(private)] | GetOwnProperty[(private)] |
      ArgumentCount | Argument | LinkTimeConstant | LinkedFunction | PublicGlobal | PrivateGlobal | ReplaceablePrivateGlobal | PrivateName |
      Macro | Define | Directive
      The front end needs the number N; the engine's name for it is the expression. The family's .cpp expands
      FOR_EACH_AOT_TYPED_<FAMILY>_CONSTANT(v) with v(N, (EXPRESSION)) into a static_assert for each.
      src/codegen/builtins-manifest.ts fails if src/js spells a name in a position that has no line.
  twin HOLDER.prototype.NAME | twin HOLDER.NAME
      The holder has the same function under the private name `@NAME` too. So `o.$NAME(..)` on an instance that has its original structure
      is a call of that function. Nobody can put another in its way: no program can spell the name.
  hardened NAME = WHAT
      The same for a free identifier NAME that JavaScriptCore is to read as `@NAME` (globalsToPrefix in src/codegen/replacements.ts).
  omitted FILE.FUNCTION
      The function of builtins/FILE.js is not compiled: nothing asks for it where there is no JIT.
  written FILE
      <bun>/src/aot/prelude/engine/FILE.ts is written by hand and has every function of builtins/FILE.js.
  constant HOLDER.NAME = C++ EXPRESSION          a number
  constant HOLDER.NAME : symbol
  accessor NAME : read|write SIZE signed|unsigned|float little|big
      A method of the embedder's Buffer that moves SIZE bytes at an offset: what runtime/BufferAccessorRegistry.h says of the host function. The
      table has no entry for it and the checker is told nothing: NAME is among the names, and a row of FOR_EACH_AOT_TYPED_BUFFER_ACCESSOR.
      Compiled code takes the row to hold only of a function whose registered descriptor says the same. One whose width is an argument
      (readUIntLE) has no line.
  header TEXT
      A header name for which the embedder's HTTPHeaderName has an enumerator: TEXT without its dashes. The lines stand in the order of the
      enumerators: the first is number 0. A row of FOR_EACH_AOT_TYPED_HTTP_HEADER_NAME: enumerator, TEXT in lower case, number.
  KIND PATH [this/USES] (PARAMETERS) -> RESULT ; EFFECTS ; LOWERING

  KIND       method | getter | setter    PATH is HOLDER.prototype.NAME
             static                      PATH is HOLDER.NAME
             function                    PATH is the name of a global function
             call | construct            PATH is the name of a global constructor, called or constructed
             operation                   PATH is the name by which the front end asks for it
             intrinsic                   the same; the prelude writes it `$name(..)`
  HOLDER     a global name, or %TypedArray%. NAME may be @@iterator and the like.
  PARAMETERS comma-separated: PATTERN, PATTERN? (optional), PATTERN... (rest); after `any` and `TypedArray!` there may be /USES
  PATTERN    any | number! | int! | TypedArray! | toNumber | toString | toPropertyKey
             `x!`: the entry applies only if the static type of the argument is within x. `toX`: only if ToX of it runs no user code.
  USES       What the built-in may do with the operand, for the escape analyses of the checker and of the engine (OperandUse in
             AOTTypedBuiltins.h). The checker is not told of an `operation`. `-`: it only looks at it. Else letters:
             W  creates, replaces or deletes own named properties of it, or makes one an accessor
             w  writes its elements, length, internal contents (the entries of a Map, the bytes of a buffer, lastIndex), prototype,
                extensibility; freezes or seals it
             K  keeps the value itself somewhere that outlives the call, the result included
             C  calls it, now or later
             G  reads it by [[Get]], asks for its keys, converts it, iterates over it, looks for `then` or a species: user code runs only if
                the object has an accessor, a trap or such a method
             R  the result may be the value itself
             P  what it holds (values of properties, elements, entries, its prototype) is kept, returned or handed on
             A  it is handed to a callback, or to code that G of another operand runs, as an argument or as `this`
             D  with the letters beside it: of everything that can be reached from it
             It has to be true of the built-in whenever the patterns fit and the receiver is of the holder's class, whatever path the
             engine takes. Nothing written: nothing, if the holder is a class of primitives, after `number!`, or if the entry neither calls
             user code nor writes anything nor reads its arguments and yields a primitive (G for a receiver it reads). G after `toX`: that
             is what the built-in does to an object there, where the entry does not apply. Else EVERYTHING.
  RESULT     ALTERNATIVE {| ALTERNATIVE}: anything | undefined | null | boolean | number | string | symbol | bigint | this | argN |
             int[LOW,HIGH] | new CLASS | new same
             `new CLASS` is an instance of the class. With /N, also after `anything`: made by the call, and held by nothing else.
             With /S: the same, unless the receiver's `constructor` leads to a species (ArraySpeciesCreate).
  EFFECTS    Pure | CallsUserCode | one or more of MayThrow Allocates ReadsReceiver WritesReceiver ReadsArguments WritesArguments
             ReadsRuntimeState WritesRuntimeState
  LOWERING   Generic            the site stays a call by name
             Call               the function is known, and is called like any function value (it is written in JavaScript)
             HostCall           the function is a host function or an InternalFunction: a direct native call
             Inline(NAME)       InlineBuiltin::NAME; where that has no code for the arguments, as HostCall or Call: Inline(NAME, Call)
                                Of a static function: it does what the function does on its holder, AND on `undefined` and `null`: a
                                constant that holds the function is called so (`const of = Array.of; of(1)`). Not of Promise and
                                %TypedArray%, whose functions throw there: theirs is only taken on the holder
                                Inline(NAME, None): there is no function, the code covers every case (an own property of every instance)
             Operation(NAME)    LanguageOperation::NAME
             Prelude(FUNCTION)  `export function FUNCTION` of <bun>/src/aot/prelude/<family>.ts, called directly and inlined. Of a method its
                                first parameter is the receiver. Not of a getter or a setter, nor of a static function of %TypedArray%
             Number(C++ EXPRESSION)

The entries of one built-in are adjacent, and the first that applies to the arguments is chosen."""
import glob, os, re, sys
import aot_engine_builtins

here = os.path.dirname(os.path.abspath(__file__))
aot = os.path.join(os.path.dirname(here), 'aot')
arguments = sys.argv[1:]
check = '--check' in arguments
bun = arguments[arguments.index('--bun') + 1] if '--bun' in arguments else None

KINDS = {'method': 'Method', 'getter': 'Getter', 'setter': 'Setter', 'static': 'StaticFunction', 'function': 'Function', 'call': 'Function', 'construct': 'Constructor', 'operation': 'Operation', 'intrinsic': 'Operation'}
PATTERNS = {'any': 'asIs', 'number!': 'isNumber', 'int!': 'isInteger', 'TypedArray!': 'isTypedArray', 'toNumber': 'toNumber', 'toString': 'toString', 'toPropertyKey': 'toPropertyKey'}
EFFECTS = ['MayThrow', 'Allocates', 'ReadsReceiver', 'WritesReceiver', 'ReadsArguments', 'WritesArguments', 'ReadsRuntimeState', 'WritesRuntimeState']
ENTRY = re.compile(r'(\w+) (\S+)(?: this/(\S+))? \((.*?)\) -> (.*?) ; (.*?) ; (.*)')
USES = 'WwKCGRPAD'
EVERYTHING = (1 << len(USES)) - 1

entries, classes, structures, replaceable, predicted, twins, errors = [], [], [], [], [], [], []
families, inline_builtins, runtime_operations, intrinsics, hardened = [], {}, [], [], set()
omitted, written = [], []
FAMILY_NAMES = {'regexp': 'RegExp', 'typedarray': 'TypedArray', 'error-json-global': 'ErrorJSONGlobal', 'json': 'JSON', 'json-parse': 'JSONParse'}
WRITES = ['Pure', 'Nothing', 'RuntimeState', 'TypedArrayProperties', 'Fields', 'Heap']


buffer_accessors = {}
http_header_names = []


def family_name(family):
    return FAMILY_NAMES.get(family) or ''.join(part.capitalize() for part in family.split('-'))


def split_top_level(text):
    parts, depth, current = [], 0, ''
    for character in text:
        depth += character == '('
        depth -= character == ')'
        if character == ',' and not depth:
            parts.append(current.strip())
            current = ''
        else:
            current += character
    return parts + [current.strip()] if current.strip() else parts


def add_class(name, listed_as, typed_array_type, js_type, own):
    classes.append((name, listed_as, typed_array_type, js_type, len(structures), len(own)))
    structures.extend(own)


def parse_operand_uses(where, text):
    if text is None or text == '-':
        return None if text is None else 0
    if not text or any(letter not in USES for letter in text) or sorted(text, key=USES.index) != list(text) or len(set(text)) != len(text) or text == 'D':
        errors.append('%s: uses %s: letters of %s, in that order, each once, D not alone' % (where, text, USES))
        return None
    return sum(1 << USES.index(letter) for letter in text)


def add_entry(where, family, kind, path, parameters, result, effects, lowering, this=None):
    uses = []
    for index, parameter in enumerate(parameters):
        match = re.fullmatch(r'([^/]+)/([^?.]+)(\?|\.\.\.)?', parameter)
        uses.append(parse_operand_uses(where, match.group(2)) if match else None)
        if match:
            parameters[index] = match.group(1) + (match.group(3) or '')
            if match.group(1) not in ('any', 'TypedArray!'):
                errors.append('%s: %s admits primitives only: no uses' % (where, match.group(1)))
    alternatives = [alternative.strip() for alternative in result.split('|')]
    objects = [alternative for alternative in alternatives if alternative.startswith(('new ', 'anything', 'this', 'arg'))]
    marks = {alternative[-1] if alternative[-2:] in ('/N', '/S') else '' for alternative in objects}
    if len(marks) > 1 or any(alternative[-2:] in ('/N', '/S') for alternative in alternatives if alternative not in objects):
        errors.append('%s: result %s: /N or /S on every alternative that may be an object, or on none' % (where, result))
    fresh = {'N': 'Yes', 'S': 'UnlessSpecies'}.get(''.join(marks), 'No')
    result = ' | '.join(alternative.removesuffix('/N').removesuffix('/S') for alternative in alternatives)
    holder, name = '', path
    if kind in ('method', 'getter', 'setter'):
        holder, separator, name = path.partition('.prototype.')
        if not separator:
            errors.append('%s: %s is not HOLDER.prototype.NAME' % (where, path))
    elif kind in ('static', 'constant'):
        holder, _, name = path.rpartition('.')
    holder, name = (text.replace('%TypedArray%', 'TypedArray') for text in (holder, name))
    entries.append(dict(where=where, family=family, kind=kind, holder=holder, name=name, parameters=parameters, result=result, effects=effects, lowering=lowering,
                        this=parse_operand_uses(where, this), uses=uses, fresh=fresh))


for path in sorted(glob.glob(os.path.join(aot, 'builtins', '*.def'))):
    family = os.path.basename(path)[:-4]
    families.append(family)
    for number, raw in enumerate(open(path), 1):
        line = raw.split('#')[0].strip()
        where = '%s.def:%d' % (family, number)
        if not line:
            continue
        match = re.fullmatch(r'class (\w+) : (\w+)(?: = (.*))?', line)
        if match:
            add_class(match.group(1), match.group(1), 'NotTypedArray', match.group(2), split_top_level(match.group(3) or ''))
            continue
        match = re.fullmatch(r'typedarray (\w+)', line)
        if match:
            name = match.group(1)
            add_class(name + 'Array', 'TypedArray', 'Type' + name, name + 'ArrayType', ['typedArrayStructure(Type%s, %s)' % (name, resizable) for resizable in ('false', 'true')])
            add_entry(where, family, 'constant', name + 'Array.BYTES_PER_ELEMENT', [], 'number', ['Pure'], 'Number(elementSize(Type%s))' % name)
            add_entry(where, family, 'getter', name + 'Array.prototype.BYTES_PER_ELEMENT', [], 'number', ['Pure'], 'Number(elementSize(Type%s))' % name)
            continue
        match = re.fullmatch(r'replaceable (\w+)', line)
        if match:
            replaceable.append(match.group(1))
            continue
        match = re.fullmatch(r'predicted ([\w.]+)', line)
        if match:
            predicted.append((where, match.group(1)))
            continue
        match = re.fullmatch(r'omitted (\w+)\.(\w+)', line)
        if match:
            omitted.append((where,) + match.groups())
            continue
        match = re.fullmatch(r'written (\w+)', line)
        if match:
            written.append((where, match.group(1)))
            continue
        match = re.fullmatch(r'inline (\w+)( exact)?', line)
        if match:
            if match.group(1) in inline_builtins:
                errors.append('%s: inline %s is declared twice' % (where, match.group(1)))
            inline_builtins[match.group(1)] = (family, bool(match.group(2)))
            continue
        match = re.fullmatch(r'runtime (\w+) : (\w+) ([\w* ]+?) \((.*)\)( noexcept( leaf)?)?', line)
        if match:
            if match.group(2) not in WRITES:
                errors.append('%s: writes %s' % (where, match.group(2)))
            runtime_operations.append(match.groups())
            continue
        match = re.fullmatch(r'(?:\$(\w+?)(\*)? (Call|Value|Member)|hardened (\w+)) = (\w+)(?:\((.*)\))?', line)
        if match:
            intrinsics.append((where, match.group(1) or match.group(4), bool(match.group(2)), match.group(3) or 'Value', match.group(5), match.group(6)))
            if match.group(4):
                hardened.add(match.group(4))
            continue
        match = re.fullmatch(r'twin (\w+)(\.prototype)?\.(\w+)', line)
        if match:
            twins.append((where, match.group(1), bool(match.group(2)), match.group(3)))
            continue
        match = re.fullmatch(r'constant (\S+) = (.*)', line)
        if match:
            add_entry(where, family, 'constant', match.group(1), [], 'number', ['Pure'], 'Number(%s)' % match.group(2))
            continue
        match = re.fullmatch(r'constant (\S+) : symbol', line)
        if match:
            add_entry(where, family, 'constant', match.group(1), [], 'symbol', ['Pure'], 'Cell')
            continue
        match = re.fullmatch(r'accessor (\w+) : (read|write) ([1248]) (signed|unsigned|float) (little|big)', line)
        if match:
            if match.group(1) in buffer_accessors:
                errors.append('%s: accessor %s is declared twice' % (where, match.group(1)))
            buffer_accessors[match.group(1)] = match.groups()[1:]
            continue
        match = re.fullmatch(r'header ([A-Za-z0-9]+(?:-[A-Za-z0-9]+)*)', line)
        if match:
            if match.group(1).lower() in (name.lower() for name in http_header_names):
                errors.append('%s: header %s is declared twice' % (where, match.group(1)))
            http_header_names.append(match.group(1))
            continue
        match = ENTRY.fullmatch(line)
        if not match or match.group(1) not in KINDS:
            errors.append('%s: not a declaration: %s' % (where, line))
            continue
        kind, path_of_entry, this, parameters, result, effects, lowering = match.groups()
        add_entry(where, family, kind, path_of_entry, [p.strip() for p in parameters.split(',') if p.strip()], result, effects.split(), lowering, this)


def cpp_parameter(where, parameter):
    pattern = parameter.rstrip('?').rstrip('.')
    if pattern not in PATTERNS:
        errors.append('%s: parameter %s' % (where, parameter))
        return ''
    return 'optional(%s)' % PATTERNS[pattern] if parameter.endswith('?') else 'rest(%s)' % PATTERNS[pattern] if parameter.endswith('...') else PATTERNS[pattern]


engine_classes, class_numbers = [], {}


def engine_class(text, listed_as=None, typed_array_type='NotTypedArray'):
    if text not in class_numbers:
        class_numbers[text] = len(engine_classes)
        engine_classes.append(dict(text=text, listed_as=listed_as or text, typed_array_type=typed_array_type, type='CellType', first=0, count=0))
    return class_numbers[text]


for name, listed_as, typed_array_type, js_type, first, count in classes:
    engine_classes[engine_class(name, listed_as, typed_array_type)].update(type=js_type, first=first, count=count)

RESULT_KINDS = {'undefined': 'Undefined', 'null': 'Null', 'boolean': 'Boolean', 'number': 'Number', 'string': 'String', 'symbol': 'Symbol', 'bigint': 'BigInt', 'this': 'This', 'new same': 'InstanceOfSameClass'}


def cpp_result(where, result):
    kinds, made, passed, least, most = [], 0, 0, '0', '0'
    alternatives = [part.strip() for part in result.split('|')]
    for alternative in alternatives:
        integer = re.fullmatch(r'int\[(.+),(.+)\]', alternative)
        kind = None
        if integer:
            kind, (least, most) = 'Integer', integer.groups()
        elif re.fullmatch(r'arg\d', alternative):
            passed |= 1 << int(alternative[3:])
        elif alternative in RESULT_KINDS:
            kind = RESULT_KINDS[alternative]
        elif re.fullmatch(r'new \w+', alternative):
            kind, made = 'Instance', engine_class(alternative[4:])
        elif alternative != 'anything':
            errors.append('%s: result %s' % (where, alternative))
        if kind in kinds:
            errors.append('%s: result %s twice: there is room for one range and for one class' % (where, alternative))
        if kind:
            kinds.append(kind)
    if 'anything' in alternatives and len(alternatives) > 1:
        errors.append('%s: anything has no alternative' % where)
    return '{ %s, %d, %d, %s, %s }' % (' | '.join('Result::' + kind for kind in kinds) or '0', made, passed, least, most)


def cpp_effects(where, effects):
    if effects in (['Pure'], ['CallsUserCode']):
        return 'Effect::Pure' if effects == ['Pure'] else 'Effect::ClobbersWorld'
    for effect in effects:
        if effect not in EFFECTS:
            errors.append('%s: effect %s' % (where, effect))
    return ' | '.join('Effect::' + effect for effect in EFFECTS if effect in effects)


def enumerator_name(function):
    return ''.join(part[:1].upper() + part[1:] for part in function.split('$'))


operations, prelude_functions, numbers = [], [], []


def append_unique(names, name):
    if name not in names:
        names.append(name)


# These constructors are JSFunctions with a NativeExecutable, not InternalFunctions.
HOST_FUNCTION_CONSTRUCTORS = ('Boolean', 'Number', 'String')


def cpp_call(entry, how):
    if how == 'None':
        return 'BuiltinCallee::None'
    if how == 'Call':
        return 'BuiltinCallee::Function'
    if entry['kind'] not in ('call', 'construct'):
        return 'BuiltinCallee::HostFunction'
    if entry['name'] not in HOST_FUNCTION_CONSTRUCTORS:
        return 'BuiltinCallee::InternalFunction'
    return 'BuiltinCallee::HostFunction' if entry['kind'] == 'call' else 'BuiltinCallee::Function'


def cpp_lowering(entry):
    where, kind, lowering = entry['where'], entry['kind'], entry['lowering']
    is_function = kind not in ('operation', 'intrinsic', 'constant')
    if lowering == 'Generic' and is_function:
        return 'generic'
    if entry['holder'] in replaceable and (kind != 'static' or not re.fullmatch(r'Call|HostCall|Inline\(\w+\)', lowering)):
        errors.append('%s: %s is replaceable: a static function that is HostCall, Call, Generic or Inline(NAME)' % (where, entry['holder']))
    if lowering in ('Call', 'HostCall') and is_function:
        return 'calls(%s)' % cpp_call(entry, lowering)
    if lowering == 'Cell' and kind == 'constant':
        return 'cell'
    match = re.fullmatch(r'Inline\((\w+)(?:, (Call|None))?\)', lowering)
    if match:
        if match.group(1) not in inline_builtins:
            errors.append('%s: no family declares `inline %s`' % (where, match.group(1)))
        return 'inlined(InlineBuiltin::%s, %s)' % (match.group(1), cpp_call(entry, match.group(2) or ('HostCall' if is_function else 'None')))
    match = re.fullmatch(r'Operation\((\w+)\)', lowering)
    if match and kind in ('operation', 'intrinsic'):
        append_unique(operations, match.group(1))
        return 'does(LanguageOperation::%s)' % match.group(1)
    match = re.fullmatch(r'Prelude\(([\w$]+)\)', lowering)
    if match and (kind == 'method' or (kind in ('static', 'function', 'call', 'construct') and entry['holder'] != 'TypedArray')):
        append_unique(prelude_functions, (match.group(1), entry['family'], entry['name']))
        return 'prelude(PreludeFunction::%s)' % enumerator_name(match.group(1))
    match = re.fullmatch(r'Number\((.*)\)', lowering)
    if match and kind in ('constant', 'getter'):
        numbers.append((entries.index(entry), match.group(1)))
        return 'number'
    errors.append('%s: lowering %s of a %s' % (where, lowering, kind))
    return ''


for where, holder, is_of_prototype, name in twins:
    found = [entry for entry in entries if (entry['holder'], entry['name']) == (holder, name) and entry['kind'] in (('method', 'getter') if is_of_prototype else ('static',))]
    if not found:
        errors.append('%s: twin: the table has no such function' % where)
    for entry in found:
        entry['has_private_twin'] = True

PRIMITIVE_HOLDERS = ('String', 'Number', 'Boolean', 'Symbol', 'BigInt')
GETS = 1 << USES.index('G')


def operand_uses(entry):
    effects = entry['effects']
    returns_primitive = not any(alternative.strip().startswith(('new ', 'anything', 'this', 'arg')) for alternative in entry['result'].split('|'))
    is_harmless = returns_primitive and not any(effect in effects for effect in ('CallsUserCode', 'WritesReceiver', 'WritesArguments', 'WritesRuntimeState'))
    accepts_this = entry['kind'] in ('method', 'getter', 'setter') and entry['holder'] not in PRIMITIVE_HOLDERS
    if entry['this'] is not None and not accepts_this:
        errors.append('%s: it takes no `this` that is an object' % entry['where'])
    this = 0 if not accepts_this else entry['this'] if entry['this'] is not None else (GETS if 'ReadsReceiver' in effects else 0) if is_harmless else EVERYTHING
    parameters = []
    for parameter, uses in zip(entry['parameters'], entry['uses']):
        pattern = parameter.rstrip('?').rstrip('.')
        if uses is None:
            uses = (0 if pattern in ('number!', 'int!') else GETS) if pattern not in ('any', 'TypedArray!') else 0 if is_harmless and 'ReadsArguments' not in effects else EVERYTHING
        parameters.append(uses)
    return this, parameters


KIND_NUMBERS = ['Function', 'Constructor', 'StaticFunction', 'Constant', 'Method', 'Getter', 'Setter', 'Operation']
rows, listing, seen, previous, pool, runs = [], [], set(), None, [], {}
for entry in entries:
    kind = entry['table_kind'] = 'Constant' if entry['kind'] == 'constant' else KINDS[entry['kind']]
    key = (kind, entry['holder'], entry['name'])
    if key != previous and key in seen:
        errors.append('%s: the entries of %s %s.%s are not adjacent' % (entry['where'], kind, entry['holder'], entry['name']))
    entry['is_first'] = key != previous
    seen.add(key)
    previous = key
    entry['uses_of_this'], entry['uses_of_parameters'] = operand_uses(entry) if entry['kind'] != 'constant' else (0, [])
    parameters = tuple('uses(%s, %d)' % (cpp_parameter(entry['where'], parameter), uses) for parameter, uses in zip(entry['parameters'], entry['uses_of_parameters']))
    if parameters not in runs:
        runs[parameters] = len(pool)
        pool.extend(parameters)
    result = cpp_result(entry['where'], entry['result'])
    if kind in ('Method', 'Getter', 'Setter'):
        engine_class(entry['holder'])
    rows.append((entry, '{ %d, %d }, %s, %s, %d, %s' % (runs[parameters], len(parameters), result, cpp_effects(entry['where'], entry['effects']), entry['uses_of_this'], cpp_lowering(entry) + (', true' if entry.get('has_private_twin') else ''))))
    listing.append('%s %s %s (%s) -> %s ; %s ; %s' % (kind, entry['holder'] or '-', entry['name'], ', '.join(entry['parameters']), entry['result'], ' '.join(entry['effects']), entry['lowering']))

WELL_KNOWN_NAMES = ['TypedArray', 'prototype', 'call', 'apply', 'hasOwn', 'hasOwnProperty', 'propertyIsEnumerable']
WELL_KNOWN_CLASSES = ['Array', 'Promise', 'String', 'Number', 'Boolean', 'Symbol', 'BigInt', 'Function', 'Object']
WELL_KNOWN_OPERATIONS = ['HasOriginalStructure', 'Call', 'PropertyIsEnumerable']
builtin_names = set(WELL_KNOWN_NAMES) | set(replaceable)
for entry in entries:
    builtin_names.update(text for text in (entry['holder'], entry['name']) if text)
for engine in engine_classes:
    builtin_names.update((engine['text'], engine['listed_as']))
builtin_names.update(buffer_accessors)
builtin_names = sorted(builtin_names, key=lambda text: text.encode())
name_numbers = {text: number for number, text in enumerate(builtin_names)}
for text in WELL_KNOWN_CLASSES:
    if text not in class_numbers:
        errors.append('no entry makes %s a class' % text)

is_global = set()
for entry in entries:
    if entry['table_kind'] in ('Function', 'Constructor'):
        is_global.add(entry['name'])
    elif entry['table_kind'] in ('StaticFunction', 'Constant'):
        is_global.add(entry['holder'])
is_global -= set(replaceable) | {'TypedArray'}
may_be_extended = {engine['text'] for engine in engine_classes if engine['type'] not in ('CellType', 'ArrayType')}
# The only class with a method of the name, among those whose instances one test tells.
speculated = {}
for entry in entries:
    if entry['table_kind'] != 'Method' or entry['lowering'] == 'Generic':
        continue
    engine = engine_classes[class_numbers[entry['holder']]]
    if engine['text'] == 'String' or engine['type'] == 'ArrayType' or engine['count']:
        speculated[entry['name']] = 'multipleClasses' if speculated.get(entry['name'], engine['text']) != engine['text'] else engine['text']


def cpp_name(text):
    return 'Name(%d)' % name_numbers[text] if text else 'Name()'


def array(declaration, lines):
    return 'constexpr %s = {\n%s};\n\n' % (declaration, ''.join('    %s,\n' % line for line in lines))


operation_numbers = {entry['name']: number for number, entry in enumerate(entries) if entry['table_kind'] == 'Operation' and entry['is_first']}
first_entries = sorted((KIND_NUMBERS.index(entry['table_kind']) << 32 | (name_numbers[entry['holder']] if entry['holder'] else 0xffff) << 16 | name_numbers[entry['name']], number, entry)
                       for number, entry in enumerate(entries) if entry['is_first'])
table = array('BuiltinParameter builtinParameterPool[]', pool)
table += array('Builtin builtinEntries[numberOfBuiltins]', ['{ BuiltinKind::%s, "%s"_s, "%s"_s, %s, %s, %s }' % (entry['table_kind'], entry['holder'], entry['name'], cpp_name(entry['holder']), cpp_name(entry['name']), rest)
                                                            for entry, rest in rows])
table += array('BuiltinNameData builtinNames[numberOfBuiltinNames]', ['{ "%s"_s, %s, %s, %s, %s, %s, %s }' % (
    text, class_numbers.get(text, 'noClass'), class_numbers.get(speculated[text], 'multipleClasses') if text in speculated else 'noClass', operation_numbers.get(text, 'noOperation'),
    *('true' if text in texts_with else 'false' for texts_with in (is_global, replaceable, may_be_extended))) for text in builtin_names])
table += array('BuiltinClass builtinClasses[numberOfEngineClasses]', ['{ "%s"_s, %d, %s, %s, %s, %s, %d, %d }' % (
    engine['text'], number, cpp_name(engine['text']), cpp_name(engine['listed_as']), engine['typed_array_type'], engine['type'], engine['first'], engine['count']) for number, engine in enumerate(engine_classes)])
table += array('FirstBuiltin firstBuiltins[]', ['{ keyFor(BuiltinKind::%s, %s, %s), %d }' % (entry['table_kind'], cpp_name(entry['holder']), cpp_name(entry['name']), number) for key, number, entry in first_entries])
captured = [str(number) for number, entry in enumerate(entries) if entry['holder'] in replaceable and entry['lowering'] != 'Generic']
table += 'constexpr std::array<uint16_t, %d> cellsOfReplaceableGlobalsData { %s };\n\n' % (len(captured), ', '.join(captured))
table += 'static double numberOfBuiltin(unsigned index)\n{\n    switch (index) {\n%s    }\n    RELEASE_ASSERT_NOT_REACHED();\n}\n' % ''.join('    case %d:\n        return %s;\n' % number for number in numbers)

names = {}
for entry in entries:
    if entry['kind'] == 'operation':
        continue
    holder, name = entry['holder'], entry['name']
    if entry['kind'] == 'intrinsic':
        name = '$' + name[0].lower() + name[1:]
    if any(holder == declared[0] and declared[1] == 'TypedArray' for declared in classes) and name == 'BYTES_PER_ELEMENT':
        holder = 'TypedArray'
    names.setdefault(holder, set()).add(name)

for where, path in predicted:
    holder, _, name = path.rpartition('.')
    if name not in names.get(holder, ()):
        errors.append('%s: predicted %s: the table has no such name' % (where, path))
        continue
    names[holder].discard(name)
    if not holder:
        names.pop(name, None)
    elif not names[holder]:
        del names[holder]

# What the checker is told of each entry.
RUST_KINDS = {'function': 'Function', 'call': 'Call', 'construct': 'Construct', 'static': 'Static', 'method': 'Method', 'getter': 'Getter', 'setter': 'Setter', 'intrinsic': 'Intrinsic'}
RUST_PATTERNS = {'any': 'Any', 'number!': 'Number', 'int!': 'Integer', 'TypedArray!': 'TypedArray', 'toNumber': 'ToNumber', 'toString': 'ToString', 'toPropertyKey': 'ToPropertyKey'}
uses_rows = []
for entry in entries:
    if entry['kind'] not in RUST_KINDS:
        continue
    this = entry['uses_of_this']
    parameters = []
    for parameter, uses in zip(entry['parameters'], entry['uses_of_parameters']):
        pattern = parameter.rstrip('?').rstrip('.')
        parameters.append('(Pattern::%s, Arity::%s, %d)' % (RUST_PATTERNS.get(pattern, 'Any'), 'Optional' if parameter.endswith('?') else 'Rest' if parameter.endswith('...') else 'Required', uses))
    name = '$' + entry['name'][0].lower() + entry['name'][1:] if entry['kind'] == 'intrinsic' else entry['name']
    uses_rows.append('BuiltinEntry { kind: BuiltinKind::%s, holder: "%s", name: "%s", this: %d, parameters: &[%s], fresh: Fresh::%s }' % (
        RUST_KINDS[entry['kind']], entry['holder'], name, this, ', '.join(parameters), entry['fresh']))

prelude_files = []
for function, family, name in prelude_functions:
    append_unique(prelude_files, family)
if bun:
    for family in prelude_files:
        path = os.path.join(bun, 'src', 'aot', 'prelude', family + '.ts')
        text = open(path).read() if os.path.exists(path) else ''
        for function, of_family, name in prelude_functions:
            if of_family == family and not re.search(r'^export function %s\b' % re.escape(function), text, re.M):
                errors.append('%s.def: src/aot/prelude/%s.ts has no `export function %s`' % (family, family, function))
javascriptcore = os.path.dirname(aot)
all_engine_functions = aot_engine_builtins.functions(javascriptcore)
for where, file, function in omitted:
    if not any((file, function) == row[:2] for row in all_engine_functions):
        errors.append('%s: builtins/%s.js has no function %s' % (where, file, function))
for where, file in written:
    if not any(file == row[0] for row in all_engine_functions):
        errors.append('%s: there is no builtins/%s.js' % (where, file))
excluded_functions = {(file, function) for where, file, function in omitted}
written_files = {file for where, file in written}
engine_functions = [row for row in all_engine_functions if row[:2] not in excluded_functions]
engine_files = []
for path in aot_engine_builtins.sources(javascriptcore):
    file = os.path.basename(path)[:-3]
    if file not in written_files and any(file == row[0] for row in engine_functions):
        engine_files.append((file + '.js', aot_engine_builtins.derive(open(path).read(), {function for of_file, function in excluded_functions if of_file == file})))
if bun:
    for file in sorted(written_files):
        path = os.path.join(bun, 'src', 'aot', 'prelude', 'engine', file + '.ts')
        text = open(path).read() if os.path.exists(path) else ''
        for of_file, function, code, number in engine_functions:
            if of_file == file and not re.search(r'^export (async )?function %s\(' % aot_engine_builtins.exported_name(function), text, re.M):
                errors.append('engine.def: src/aot/prelude/engine/%s.ts has no `export function %s`' % (file, aot_engine_builtins.exported_name(function)))
if len(structures) > 255 or len(engine_classes) > 255 or len(entries) >= 0xfffe or len(builtin_names) >= 0xfffe:
    errors.append('too many entries, names, classes or structures')
for text in WELL_KNOWN_OPERATIONS:
    if text not in operation_numbers:
        errors.append('there is no operation %s' % text)

js_type_names, link_time_constant_names, intrinsic_rows, intrinsic_prefixes, checked_constants = [], [], {}, [], {}
BARE_INTRINSICS = ['IsNullish', 'Undefined', 'Call', 'Apply', 'ArrayPush', 'ArgumentCount', 'Argument', 'PrivateGlobal', 'ReplaceablePrivateGlobal', 'PrivateName', 'Macro', 'Define', 'Directive', 'LinkedFunction']
for where, name, is_prefix, position, what, payload in intrinsics:
    parts = [part.strip() for part in (payload or '').split(',') if part.strip()]
    holder, _, function = (payload or '').rpartition('.')
    if what in BARE_INTRINSICS and payload is None:
        variant = what
    elif what == 'Operation' and any(entry['kind'] in ('operation', 'intrinsic') and entry['name'] == payload for entry in entries):
        variant = 'Operation(Operation::%s)' % payload
    elif what == 'HasJSType' and len(parts) == 2:
        for part in parts:
            if part not in js_type_names:
                js_type_names.append(part)
        variant = 'HasJSType { first: %d, last: %d }' % tuple(map(js_type_names.index, parts))
    elif what == 'Builtin' and any(entry['kind'] == ('static' if holder else 'function') and (entry['holder'], entry['name']) == (holder, function) for entry in entries) and (holder or function) in is_global:
        variant = 'Builtin { holder: "%s", name: "%s" }' % (holder, function)
    elif what == 'Construct' and any(entry['kind'] == 'construct' and entry['name'] == payload for entry in entries) and payload in is_global:
        variant = 'Construct("%s")' % payload
    elif what == 'Int' and re.fullmatch(r'\d+, .+', payload or ''):
        number, expression = payload.split(', ', 1)
        variant = 'Int(%s)' % number
        checked_constants.setdefault(where.split('.def:')[0], []).append('%s, (%s)' % (number, expression))
    elif what in ('Bool', 'Expect') and payload in ('true', 'false'):
        variant = '%s(%s)' % (what, payload)
    elif what == 'Double' and payload == 'Infinity':
        variant = 'Double(f64::INFINITY)'
    elif what in ('PutByValDirect', 'GetOwnProperty') and payload in (None, 'private'):
        variant = '%s { private_key: %s }' % (what, 'true' if payload else 'false')
    elif what == 'LinkTimeConstant' and payload is None and not is_prefix:
        if name not in link_time_constant_names:
            link_time_constant_names.append(name)
        variant = 'LinkTimeConstant(%d)' % link_time_constant_names.index(name)
    elif what == 'PublicGlobal' and payload is None and name in is_global:
        variant = 'PublicGlobal("%s")' % name
    else:
        errors.append('%s: %s%s is not what a name can be, or the table has no such operation, function or global' % (where, what, '(%s)' % payload if payload is not None else ''))
        continue
    if is_prefix:
        intrinsic_prefixes.append((name, position, variant))
    elif intrinsic_rows.setdefault((name.encode(), position), variant) != variant:
        errors.append('%s: %s as %s is said to be two things' % (where, name, position))
POSITIONS = ['Call', 'Value', 'Member']
intrinsic_rows = sorted(intrinsic_rows.items(), key=lambda row: (row[0][0], POSITIONS.index(row[0][1])))
if errors:
    sys.exit('\n'.join(errors))
if '--list' in arguments:
    print('\n'.join(listing))
    sys.exit(0)

hashed = ''.join('%s %s\n' % (holder, name) for holder in sorted(names) for name in sorted(names[holder]))
hashed += ''.join('%s %s\n' % (name, function) for function, family, name in prelude_functions)
hashed += ''.join('%s %d\n' % (entry['name'], len(entry['parameters'])) for entry in entries if entry['kind'] == 'operation')
hashed += ''.join('%s\n' % row for row in uses_rows)
hashed += ''.join('%d %s %s\n' % (number, file, function) for file, function, code, number in engine_functions)
hashed += ''.join('$%s %s %s\n' % (name.decode(), position, variant) for (name, position), variant in intrinsic_rows)
hashed += ''.join('$%s* %s %s\n' % row for row in intrinsic_prefixes)
table_hash = 2166136261
for byte in hashed.encode():
    table_hash = ((table_hash ^ byte) * 16777619) & 0xffffffff

NOTICE = 'Generated by Source/JavaScriptCore/Scripts/generate-aot-builtins.py from Source/JavaScriptCore/aot/builtins/*.def. Do not edit.'


def x_macro(name, rows):
    return '#define %s(v)%s\n\n' % (name, ''.join(' \\\n    v(%s)' % row for row in rows))


lists = '// %s\n\n#pragma once\n\n' % NOTICE
lists += x_macro('FOR_EACH_AOT_TYPED_BUILTIN_FAMILY', map(family_name, families))
lists += x_macro('FOR_EACH_AOT_TYPED_INLINE_BUILTIN', ['%s, %s, %s' % (name, family_name(family), 'true' if is_exact else 'false') for name, (family, is_exact) in inline_builtins.items()])
lists += x_macro('FOR_EACH_AOT_TYPED_BUILTIN_OPERATION', ['operationAOTTyped' + operation[0] for operation in runtime_operations])
lists += x_macro('FOR_EACH_AOT_TYPED_LANGUAGE_OPERATION', operations)
lists += x_macro('FOR_EACH_AOT_TYPED_IR_OPERATION', ['%s, %d, %d' % (name, number, name_numbers[name]) for name, number in sorted(operation_numbers.items(), key=lambda operation: operation[1])])
lists += x_macro('FOR_EACH_AOT_TYPED_PRELUDE_FUNCTION', ['%s, "%s"' % (enumerator_name(function), function) for function, family, name in prelude_functions])
lists += x_macro('FOR_EACH_AOT_TYPED_ENGINE_BUILTIN_FUNCTION', ['%s, %d' % (code, number) for file, function, code, number in engine_functions])
lists += x_macro('FOR_EACH_AOT_TYPED_ORIGINAL_STRUCTURE', ['%d, %s' % numbered for numbered in enumerate(structures)])
lists += x_macro('FOR_EACH_AOT_TYPED_BUFFER_ACCESSOR', ['%s, %d, %s, %s, %s, %s' % (name, name_numbers[name], 'true' if direction == 'write' else 'false', size, kind.capitalize(), 'true' if order == 'little' else 'false')
                                                        for name, (direction, size, kind, order) in buffer_accessors.items()])
lists += x_macro('FOR_EACH_AOT_TYPED_HTTP_HEADER_NAME', ['%s, "%s", %d' % (name.replace('-', ''), name.lower(), number) for number, name in enumerate(http_header_names)])
lists += x_macro('FOR_EACH_AOT_TYPED_JS_TYPE_NAME', js_type_names)
lists += x_macro('FOR_EACH_AOT_TYPED_LINK_TIME_CONSTANT_NAME', link_time_constant_names)
for family in families:
    if family in checked_constants:
        lists += x_macro('FOR_EACH_AOT_TYPED_%s_CONSTANT' % family_name(family).upper(), checked_constants[family])
lists += '#define AOT_TYPED_NUMBER_OF_BUILTINS %d\n#define AOT_TYPED_NUMBER_OF_BUILTIN_NAMES %d\n#define AOT_TYPED_NUMBER_OF_ENGINE_CLASSES %d\n' % (len(entries), len(builtin_names), len(engine_classes))
lists += '#define AOT_TYPED_NUMBER_OF_ORIGINAL_STRUCTURES %d\n#define AOT_TYPED_BUILTINS_HASH 0x%08xu\n\n' % (len(structures), table_hash)
lists += ''.join('#define AOT_TYPED_BUILTIN_NAME_%s %d\n' % (text, name_numbers[text]) for text in WELL_KNOWN_NAMES)
lists += ''.join('#define AOT_TYPED_ENGINE_CLASS_%s %d\n' % (text, class_numbers[text]) for text in WELL_KNOWN_CLASSES)
lists += ''.join('#define AOT_TYPED_BUILTIN_%s %d\n' % (text, operation_numbers[text]) for text in WELL_KNOWN_OPERATIONS)

trusted = '// %s\n\n#![allow(dead_code)]\n\n' % NOTICE
trusted += '/// Every built-in the engine\'s table has an entry for, by holder, the names separated by one space. `""`: global functions, constructors and the intrinsics of the prelude. `@@name`: a well-known symbol.\n'
trusted += 'pub(super) const BUILTINS: &[(&str, &str)] = &[\n%s];\n' % ''.join('    ("%s", "%s"),\n' % (holder, ' '.join(sorted(names[holder]))) for holder in sorted(names))
trusted += 'pub(super) const REPLACEABLE_GLOBALS: &[&str] = &[%s];\n' % ', '.join('"%s"' % name for name in sorted(replaceable))
trusted += '''
/// What a built-in may do with an operand. The letters of the header of generate-aot-builtins.py, in their order.
pub(super) mod uses {
    /// Creates, replaces or deletes own named properties, or makes one an accessor.
    pub(in super::super) const WRITES_PROPERTIES: u16 = 1;
    /// Writes elements, length, internal contents, the prototype, extensibility; freezes or seals.
    pub(in super::super) const WRITES_OTHERWISE: u16 = 2;
    /// Keeps the value itself somewhere that outlives the call, the result included.
    pub(in super::super) const KEEPS: u16 = 4;
    /// Calls it, now or later.
    pub(in super::super) const CALLS: u16 = 8;
    /// [[Get]], own keys, conversions, iteration, `then`, species: user code runs only if the object has an accessor, a trap or such a method.
    pub(in super::super) const GETS: u16 = 16;
    /// The result may be the value itself.
    pub(in super::super) const RETURNS: u16 = 32;
    /// What it holds is kept, returned or handed on.
    pub(in super::super) const PARTS: u16 = 64;
    /// It is handed to a callback, or to code that `GETS` of another operand runs, as an argument or as `this`.
    pub(in super::super) const HANDS_OVER: u16 = 128;
    /// The other bits hold of everything that can be reached from it.
    pub(in super::super) const DEEP: u16 = 256;
    pub(in super::super) const EVERYTHING: u16 = 511;
}

#[derive(Copy, Clone, PartialEq, Eq, Debug)]
pub(super) enum BuiltinKind {
    /// A global function.
    Function,
    /// A global constructor, called.
    Call,
    /// A global constructor, constructed.
    Construct,
    Static,
    Method,
    Getter,
    Setter,
    /// `$name(..)` in the prelude.
    Intrinsic,
}

/// `Number`, `Integer`, `TypedArray`: the static type of the argument is within that. `ToX`: ToX of it runs no user code.
#[derive(Copy, Clone, PartialEq, Eq, Debug)]
pub(super) enum Pattern {
    Any,
    Number,
    Integer,
    TypedArray,
    ToNumber,
    ToString,
    ToPropertyKey,
}

#[derive(Copy, Clone, PartialEq, Eq, Debug)]
pub(super) enum Arity {
    Required,
    Optional,
    Rest,
}

/// Whether every object the entry can return was made by the call and is held by nothing else.
#[derive(Copy, Clone, PartialEq, Eq, Debug)]
pub(super) enum Fresh {
    No,
    /// Yes, unless the receiver's `constructor`, own or inherited, leads to a species: then code of the program makes the result.
    UnlessSpecies,
    Yes,
}

/// True of the built-in whenever the arguments fit the patterns and the receiver is of the holder's class.
pub(super) struct BuiltinEntry {
    pub(super) kind: BuiltinKind,
    /// As in `BUILTINS`.
    pub(super) holder: &'static str,
    pub(super) name: &'static str,
    pub(super) this: u16,
    pub(super) parameters: &'static [(Pattern, Arity, u16)],
    pub(super) fresh: Fresh,
}

/// The entries of one built-in are adjacent, in the order in which the engine tries them: the first that fits applies.
pub(super) const BUILTIN_ENTRIES: &[BuiltinEntry] = &[
%s];
''' % ''.join('    %s,\n' % row for row in uses_rows)

front_end = '// %s\n\n' % NOTICE
front_end += '/// `Format::Header::builtinsHash` of the engine.\npub(crate) const BUILTINS_HASH: u32 = 0x%08x;\n\n' % table_hash
front_end += '/// The files of the prelude: name and text.\npub const PRELUDE_FILES: &[(&str, &str)] = &[\n%s];\n\n' % ''.join('    ("%s.ts", include_str!("prelude/%s.ts")),\n' % (family, family) for family in prelude_files)
front_end += '/// The name of a built-in, and a function of the prelude that the engine may call in its place.\npub const PRELUDE_FUNCTIONS: &[(&str, &str)] = &[\n%s];\n' % ''.join(
    '    ("%s", "%s"),\n' % (name, function) for function, family, name in prelude_functions)
front_end += '\n#[derive(Copy, Clone, PartialEq, Eq, Hash, Debug)]\n#[repr(u16)]\npub enum Operation {\n%s}\n' % ''.join('    %s = %d,\n' % operation for operation in sorted(operation_numbers.items(), key=lambda operation: operation[1]))
front_end += '\nimpl Operation {\n    const BY_NAME: &[(&str, Operation)] = &[\n%s    ];\n' % ''.join('        ("%s", Operation::%s),\n' % (name, name) for name in sorted(operation_numbers))
front_end += '\n    pub(crate) fn name(self) -> &\'static str {\n        match self {\n%s        }\n    }\n' % ''.join('            Operation::%s => "%s",\n' % (name, name) for name in sorted(operation_numbers, key=operation_numbers.get))
front_end += '''
    pub(crate) fn named(name: &[u8]) -> Option<Operation> {
        Self::BY_NAME.binary_search_by(|entry| entry.0.as_bytes().cmp(name)).ok().map(|at| Self::BY_NAME[at].1)
    }

    pub(crate) fn numbered(number: u32) -> Option<Operation> {
        Self::BY_NAME.iter().find(|entry| entry.1 as u32 == number).map(|entry| entry.1)
    }
}
'''

intrinsics_rs = '// %s\n' % NOTICE + '''
//! What a `$name` is in a file of Bun's src/js (an internal module, a file of builtin functions) that does not declare it.

use crate::builtins_generated::Operation;

/// Where the name stands.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub(crate) enum Position {
    /// `$name(..)`
    Call,
    /// Not called. Also the callee of `new $name(..)`.
    Value,
    /// `o.$name`, `o.$name(..)`, a key of an object literal or a pattern, a member of a class.
    Member,
}

#[derive(Clone, Copy)]
pub(crate) enum Intrinsic {
    /// `Op::Operation` of that name, with the arguments as written.
    Operation(Operation),
    /// `Operation HasJSType(value, Int(first), Int(last))`: numbers in the engine's list of the JSTypes that are asked for.
    HasJSType { first: u8, last: u8 },
    /// The public function, which no program can replace: `holder.name`, or the global function `name` if there is no holder.
    Builtin { holder: &'static str, name: &'static str },
    /// `Op::Construct` of that public global with the first argument.
    Construct(&'static str),
    /// `Op::IsNullish` of the argument.
    IsNullish,
    Undefined,
    Int(i64),
    Bool(bool),
    Double(f64),
    Expect(bool),
    /// `f.$call(this, ..arguments)`: `Op::Call`. No property is read.
    Call,
    /// `f.$apply(this, list)`: `Operation Apply(f, this, list)`.
    Apply,
    /// `$arrayPush(array, value)`: `Operation PutByValDirect(array, PropGet array "length", value)`.
    ArrayPush,
    /// `Operation PutByValDirect(object, key, value)`, which yields the value. A private key is written as a string literal: the key is
    /// that text behind `PRIVATE_NAME_PREFIX`.
    PutByValDirect { private_key: bool },
    /// `Operation GetOwnProperty(object, key)`. The key is a string literal.
    GetOwnProperty { private_key: bool },
    /// The length of the array that all arguments of the function arrive in.
    ArgumentCount,
    /// `$argument(n)`: element `n` of that array.
    Argument,
    /// `Operation LinkTimeConstant(Int(number))`: its number in the engine's list of those that are asked for. Called: that, then `Op::Call`.
    LinkTimeConstant(u8),
    /// `Op::FreeGet` of that name, which no program can replace.
    PublicGlobal(&'static str),
    /// `Op::FreeGet` of the name behind `PRIVATE_NAME_PREFIX`.
    PrivateGlobal,
    ReplaceablePrivateGlobal,
    /// The key is the name behind `PRIVATE_NAME_PREFIX`.
    PrivateName,
    /// `Manifest::macro_for_call` says what the call stands for.
    Macro,
    /// The manifest has a define for `$name`.
    Define,
    /// It says something of the builtin function it is at the head of. Nothing is emitted.
    Directive,
    /// A builtin function written in JavaScript, in the same link: the one of that name with the directive `$linkTimeConstant;`. Called:
    /// `Op::CallDirect`. As a value: its closure.
    LinkedFunction,
}

/// The first byte of a string that stands for a private name of the engine. No UTF-8 text has it.
pub(crate) const PRIVATE_NAME_PREFIX: u8 = 0xFD;

/// Sorted.
const ROWS: &[(&[u8], Position, Intrinsic)] = &[
%s];

const PREFIXES: &[(&[u8], Position, Intrinsic)] = &[
%s];

/// `name` is what follows the `$`, or a free identifier that `Manifest::is_hardened_global` holds for. None: src/js has no such use.
pub(crate) fn resolve_intrinsic(name: &[u8], position: Position) -> Option<Intrinsic> {
    if let Ok(index) = ROWS.binary_search_by(|row| (row.0, row.1).cmp(&(name, position))) {
        return Some(ROWS[index].2);
    }
    PREFIXES.iter().find(|row| row.1 == position && name.starts_with(row.0)).map(|row| row.2)
}
''' % (''.join('    (b"%s", Position::%s, Intrinsic::%s),\n' % (name.decode(), position, variant) for (name, position), variant in intrinsic_rows),
       ''.join('    (b"%s", Position::%s, Intrinsic::%s),\n' % row for row in intrinsic_prefixes))

intrinsics_txt = ''.join('%s\t%s%s\n' % (name.decode(), position, '\thardened' if position == 'Value' and name.decode() in hardened else '') for (name, position), variant in intrinsic_rows)
intrinsics_txt += ''.join('%s*\t%s\n' % (name, position) for name, position, variant in intrinsic_prefixes)

declarations = ''.join('AOT_DECLARE_%sTYPED_OPERATION(operationAOTTyped%s, %s, %s, (%s));\n' % ('NOEXCEPT_' if is_noexcept else '', name, writes, result, ', '.join(filter(None, ['' if is_leaf else 'Instance*', parameters])))
                       for name, writes, result, parameters, is_noexcept, is_leaf in runtime_operations)
members = ''.join('#include "AOTTypedBuiltins%sMembers.h"\n' % family_name(family) for family in families if os.path.exists(os.path.join(aot, 'AOTTypedBuiltins%sMembers.h' % family_name(family))))
outputs = [(os.path.join(aot, 'AOTTypedBuiltinLists.h'), lists), (os.path.join(aot, 'AOTTypedBuiltinEntries.inc'), '// %s\n\n%s' % (NOTICE, table)),
           (os.path.join(aot, 'AOTTypedBuiltinOperations.inc'), '// %s\n\n%s' % (NOTICE, declarations)), (os.path.join(aot, 'AOTTypedBuiltinMembers.inc'), '// %s\n\n%s' % (NOTICE, members))]
for family in families:
    if not os.path.exists(os.path.join(aot, 'AOTTypedBuiltins%s.cpp' % family_name(family))):
        sys.exit('%s.def: there is no aot/AOTTypedBuiltins%s.cpp (it also has to be listed in Sources.txt)' % (family, family_name(family)))
    if family in checked_constants and 'FOR_EACH_AOT_TYPED_%s_CONSTANT(' % family_name(family).upper() not in open(os.path.join(aot, 'AOTTypedBuiltins%s.cpp' % family_name(family))).read():
        sys.exit('%s.def: aot/AOTTypedBuiltins%s.cpp does not hold its constants against the engine: FOR_EACH_AOT_TYPED_%s_CONSTANT' % (family, family_name(family), family_name(family).upper()))
if bun:
    outputs += [(os.path.join(bun, 'src', 'sema', 'check', 'aot', 'builtins_generated.rs'), trusted), (os.path.join(bun, 'src', 'aot', 'builtins_generated.rs'), front_end),
                (os.path.join(bun, 'src', 'aot', 'build', 'intrinsics.rs'), intrinsics_rs), (os.path.join(bun, 'src', 'aot', 'build', 'intrinsics.txt'), intrinsics_txt),
                (os.path.join(bun, 'src', 'jsc', 'bindings', 'AOTFrontEndBuiltinsHash.h'), '// %s\n\n#pragma once\n\n#define BUN_AOT_FRONT_END_BUILTINS_HASH 0x%08xu\n' % (NOTICE, table_hash)),
                (os.path.join(bun, 'src', 'jsc', 'bindings', 'AOTHTTPHeaderNames.h'), '// %s\n\n#pragma once\n\nstatic_assert(WebCore::numHTTPHeaderNames == %d);\n%s' % (NOTICE, len(http_header_names), ''.join(
                    'static_assert(static_cast<unsigned>(WebCore::HTTPHeaderName::%s) == %d);\n' % (name.replace('-', ''), number) for number, name in enumerate(http_header_names))))]
if bun:
    engine = os.path.join(bun, 'src', 'aot', 'prelude', 'engine')
    outputs += [(os.path.join(engine, name), text) for name, text in engine_files]
    outputs.append((os.path.join(engine, 'functions.txt'), ''.join(
        '%d\t%s.%s\t%s\n' % (number, file, 'ts' if file in written_files else 'js', aot_engine_builtins.exported_name(function)) for file, function, code, number in engine_functions)))
    gone = [path for path in sorted(glob.glob(os.path.join(engine, '*.js'))) if path not in dict(outputs)]
    if gone:
        sys.exit('no file of builtins/ stands behind these any more, delete them:\n' + '\n'.join(gone))
stale = [path for path, text in outputs if not os.path.exists(path) or open(path).read() != text]
if check:
    if stale:
        sys.exit('not up to date, run generate-aot-builtins.py:\n' + '\n'.join(stale))
else:
    for path, text in outputs:
        if path in stale:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            open(path, 'w').write(text)
print('%d families, %d entries, %d names under %d holders, %d classes, %d structures, %d inline, %d language operations, %d run-time operations, %d prelude functions, hash %08x%s' % (
    len(families), len(entries), sum(map(len, names.values())), len(names), len(engine_classes), len(structures), len(inline_builtins), len(operations), len(runtime_operations), len(prelude_functions), table_hash,
    '' if bun else '; the files of the front end are not looked at without --bun'))
