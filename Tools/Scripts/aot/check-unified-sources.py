#!/usr/bin/env python3
"""Check that any two sources of aot/ can share a translation unit. Takes five seconds; needs no compiler and no build.

    check-unified-sources.py [--generated <directory>]

aot/ is compiled as unified sources: a bundle includes sixteen sources one after the other, and adding a source shifts every
bundle after it. So what a source declares for itself at namespace scope (a static function or constant, anything in an
unnamed namespace, a type, an alias) has to be its own in all of aot/, not only in today's bundle. This reads the text of
every source and header and reports:

    a name that two sources declare for themselves in the same namespace (two functions may share a name if their
    parameter types differ and both or neither are in an unnamed namespace)
    a name that a source declares for itself and a header declares in the same namespace or in one around it
    a constant or a function that two headers define in the same namespace
    `using namespace` at namespace scope in a source
    a macro that two files leave defined

To fix one: give the name a more specific one, or, if the declarations are the same, keep one in a header that both include.

What only the compiler sees, in the bundle where it happens: a function of a source that hides one of a namespace around it
from a later source, and a function that is handed on by name (a comparator, a projection) while an earlier source has an
overload of it. The cure is the same.

--generated    where a build wrote the generated headers (<build>/JavaScriptCore/DerivedSources/aot): their names count too
"""
import argparse
import collections
import os
import re
import sys

JAVASCRIPTCORE = os.path.realpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "Source", "JavaScriptCore"))

TOKEN = re.compile(r"""
    (?P<space>[ \t\r\f\v]+|\\\n)
  | (?P<newline>\n)
  | (?P<comment>//(?:[^\n\\]|\\.|\\\n)*|/\*.*?\*/)
  | (?P<raw>(?:u8|u|U|L)?R"(?P<delimiter>[^ ()\\\t\v\f\n]{0,16})\(.*?\)(?P=delimiter)")
  | (?P<string>(?:u8|u|U|L)?"(?:[^"\\\n]|\\.)*"(?:_?[A-Za-z_]\w*)?)
  | (?P<character>(?:u8|u|U|L)?'(?:[^'\\\n]|\\.)+')
  | (?P<number>\.?\d(?:[eEpP][+-]|'?[\w.])*)
  | (?P<identifier>[A-Za-z_$][\w$]*)
  | (?P<punctuator><=>|<<=|>>=|\.\.\.|->\*|::|->|\+\+|--|<<|<=|>=|==|!=|&&|\|\||\+=|-=|\*=|/=|%=|&=|\|=|\^=|\#\#|\.\*|[{}\[\]()#;:?.~!+\-*/%^&|=<>,@\\`])
""", re.X | re.S)

Token = collections.namedtuple("Token", "kind text line")
Declaration = collections.namedtuple("Declaration", "name kind line scope is_local is_defined parameters text is_unnamed")

SPECIFIERS = {"static", "inline", "constexpr", "consteval", "constinit", "const", "volatile", "extern", "thread_local", "typename", "ALWAYS_INLINE", "NEVER_INLINE", "UNUSED_FUNCTION", "JS_EXPORT_PRIVATE", "NO_RETURN", "NO_RETURN_DUE_TO_CRASH", "WARN_UNUSED_RETURN", "SUPPRESS_ASAN", "SUPPRESS_TSAN"}
CLASS_KEYS = {"struct", "class", "union", "enum"}
CALLS_IN_SPECIFIERS = {"decltype", "alignas", "__attribute__", "noexcept", "__declspec", "requires"}


def lex(text):
    """Every token but white space and comments. A preprocessor directive is one token."""
    tokens = []
    position, line, at_line_start = 0, 1, True
    while position < len(text):
        match = TOKEN.match(text, position)
        if not match:
            raise ValueError(f"line {line}: {text[position:position + 20]!r}")
        kind, value = match.lastgroup, match.group()
        if value == "#" and at_line_start:
            end = position
            while True:
                newline = text.find("\n", end)
                if newline < 0:
                    end = len(text)
                    break
                if text[newline - 1] != "\\":
                    end = newline
                    break
                end = newline + 1
            value = text[position:end]
            tokens.append(Token("directive", value, line))
            line += value.count("\n")
            position = end
            continue
        if kind == "newline":
            at_line_start = True
        elif kind not in ("space", "comment"):
            at_line_start = False
            tokens.append(Token("raw" if kind == "delimiter" else kind, value, line))
        line += value.count("\n")
        position = match.end()
    return tokens


def closing(tokens, index, opener, closer):
    depth = 0
    while index < len(tokens):
        if tokens[index].kind == "punctuator":
            if tokens[index].text == opener:
                depth += 1
            elif tokens[index].text == closer:
                depth -= 1
                if not depth:
                    return index
        index += 1
    return len(tokens) - 1


def closing_angle(tokens, index):
    """The ">" of the "<" at index, or None if that is a comparison."""
    depth = 0
    while index < len(tokens):
        text = tokens[index].text if tokens[index].kind == "punctuator" else ""
        if text == "<":
            depth += 1
        elif text in (">", ">>"):
            depth -= len(text)
            if depth <= 0:
                return index
        elif text in "([{" and text:
            index = closing(tokens, index, text, ")]}"["([{".index(text)])
        elif text in (";", "}"):
            return None
        index += 1
    return None


def parameter_types(tokens):
    """The parameters without their names and default arguments."""
    parameters, current, depth = [], [], 0
    for token in tokens:
        text = token.text
        depth += text in ("(", "<", "[", "{")
        depth -= (text in (")", ">", "]", "}")) + 2 * (text == ">>")
        if text == "," and not depth:
            parameters.append(current)
            current = []
        else:
            current.append(text)
    if current:
        parameters.append(current)
    types = []
    for words in parameters:
        if "=" in words:
            words = words[:words.index("=")]
        words = [word for word in words if word not in ("const", "NOESCAPE")]
        if len(words) > 1 and re.fullmatch(r"\w+", words[-1]) and words[-2] not in ("::", "struct", "class", "unsigned", "long"):
            words = words[:-1]
        types.append(" ".join(words))
    return tuple(types)


def declarations_in(text):
    """The declarations at namespace scope, and the macros that are still defined at the end."""
    tokens = lex(text) + [Token("punctuator", ";", 0)] * 4
    declarations, scopes, macros = [], [], {}
    index, count = 0, len(tokens) - 4
    while index < count:
        token = tokens[index]
        if token.kind == "directive":
            match = re.match(r"#\s*(define|undef)\s+(\w+)", token.text)
            if match and match.group(1) == "define":
                macros[match.group(2)] = token.line
            elif match:
                macros.pop(match.group(2), None)
            index += 1
            continue
        if token.text in ("}", ";"):
            if token.text == "}" and scopes:
                scopes.pop()
            index += 1
            continue
        if token.text == "inline" and tokens[index + 1].text == "namespace":
            index += 1
            continue
        if re.fullmatch(r"[A-Z][A-Z0-9_]*_(BEGIN|END)", token.text) and tokens[index + 1].text != "(":
            index += 1
            continue
        if token.text == "namespace":
            cursor = index + 1
            while tokens[cursor].kind == "identifier" or tokens[cursor].text == "::":
                cursor += 1
            if tokens[cursor].text == "{":
                scopes.append("".join(t.text for t in tokens[index + 1:cursor]) or None)
                index = cursor + 1
                continue
        if token.text == "extern" and tokens[index + 1].kind == "string" and tokens[index + 2].text == "{":
            scopes.append("")
            index += 3
            continue

        start, cursor, has_body, has_initializer = index, index, False, False
        while cursor < count:
            text = tokens[cursor].text if tokens[cursor].kind == "punctuator" else ""
            if text == "(":
                cursor = closing(tokens, cursor, "(", ")")
            elif text == "[":
                cursor = closing(tokens, cursor, "[", "]")
            elif text == "=":
                has_initializer = True
            elif text == "{":
                head = {t.text for t in tokens[start:cursor]}
                cursor = closing(tokens, cursor, "{", "}")
                has_body = True
                if not has_initializer and "(" in head:
                    break
            elif text == ";":
                break
            cursor += 1
        scope = tuple(part for name in scopes if name for part in name.split("::"))
        declaration = declaration_of(tokens, start, cursor, has_body, scope, None in scopes)
        if declaration:
            declarations.append(declaration)
        index = cursor + 1
    return declarations, macros


def declaration_of(tokens, start, end, has_body, scope, is_in_unnamed_namespace):
    def make(index, kind, is_local=True, is_defined=True, parameters=None):
        text = " ".join(t.text for t in tokens[start:end + 1] if t.kind != "directive" and t.text not in ("static", "inline"))
        return Declaration(tokens[index].text, kind, tokens[index].line, scope, is_local, is_defined, parameters, text, is_in_unnamed_namespace)

    index = start
    is_template = False
    while tokens[index].text == "template" and tokens[index + 1].text == "<":
        close = closing_angle(tokens, index + 1)
        if close is None:
            return None
        is_template = True
        index = close + 1
    first = tokens[index].text
    if first == "using":
        if tokens[index + 1].text == "namespace":
            return make(index + 2, "using namespace")
        if tokens[index + 1].text == "enum":
            return None
        return make(index + 1, "alias") if tokens[index + 2].text in ("=", "[") else make(end - 1, "alias")
    if first == "typedef":
        return make(end - 1, "alias")
    if first in ("static_assert", "asm", "namespace"):
        return None
    is_static = is_constant = is_extern = False
    name = None
    cursor = index
    while cursor <= end:
        token = tokens[cursor]
        text = token.text
        if text in CLASS_KEYS:
            probe = cursor + 1 + (text == "enum" and tokens[cursor + 1].text in ("class", "struct"))
            while tokens[probe].text in ("alignas", "__attribute__", "["):
                probe = (closing(tokens, probe, "[", "]") if tokens[probe].text == "[" else closing(tokens, probe + 1, "(", ")")) + 1
            while tokens[probe].kind == "identifier" and tokens[probe + 1].kind == "identifier" and tokens[probe + 1].text != "final":
                probe += 1
            if tokens[probe].kind != "identifier":
                return None
            after = probe + 1
            if tokens[after].text in ("<", "::"):
                return None  # a specialization, or a member of another class
            if tokens[after].text in ("{", ":", "final", ";"):
                return make(probe, "type", is_defined=has_body)
        if text == "static":
            is_static = True
        elif text == "extern":
            is_extern = True
        elif text in ("constexpr", "const"):
            is_constant = True
        elif text == "operator":
            return None
        if token.kind == "identifier":
            if text in CALLS_IN_SPECIFIERS and tokens[cursor + 1].text == "(":
                cursor = closing(tokens, cursor + 1, "(", ")") + 1
                continue
            if text not in SPECIFIERS:
                name = cursor
                if tokens[cursor + 1].text == "<":
                    close = closing_angle(tokens, cursor + 1)
                    if close is not None and close <= end:
                        if tokens[close + 1].text in ("=", "{", ";", "("):
                            return None  # a specialization
                        cursor = close + 1
                        continue
        elif text == "[" and tokens[cursor + 1].text == "[":
            cursor = closing(tokens, cursor, "[", "]") + 1
            continue
        elif text in ("(", "=", "{", ";", "["):
            if name is None or tokens[name - 1].text == "::" or (text == "(" and cursor != name + 1):
                return None
            if text != "(":
                return make(name, "constant", is_local=(is_static or is_constant or is_in_unnamed_namespace) and not is_extern, is_defined=not is_extern)
            if name == index or re.fullmatch(r"[A-Z][A-Z0-9]*_[A-Z0-9_]+", tokens[name].text):
                return None  # a macro
            return make(name, "function", is_local=is_static or is_in_unnamed_namespace, is_defined=has_body, parameters=parameter_types(tokens[cursor + 1:closing(tokens, cursor, "(", ")")]))
        cursor += 1
    return None


def read(path):
    with open(path, errors="replace") as file:
        return file.read()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--generated")
    arguments = parser.parse_args()

    sources = [line.split()[0] for name in ("Sources.txt", "SourcesCompiledForSize.txt", "SourcesCompiledForMinimumSize.txt") for line in read(os.path.join(JAVASCRIPTCORE, "aot", name)).splitlines() if line.strip()]
    headers = []
    for folder, folders, names in os.walk(os.path.join(JAVASCRIPTCORE, "aot")):
        folders[:] = sorted(name for name in folders if name not in ("tests", "generator", "node_modules"))
        headers += [os.path.relpath(os.path.join(folder, name), JAVASCRIPTCORE) for name in sorted(names) if name.endswith(".h")]
    if arguments.generated:
        headers += [os.path.join(arguments.generated, name) for name in sorted(os.listdir(arguments.generated)) if name.endswith(".h")]

    twice = [path for path, count in collections.Counter(sources).items() if count > 1]
    sources = sorted(set(sources))

    own = collections.defaultdict(list)
    shared = collections.defaultdict(list)
    macros = collections.defaultdict(list)
    problems = [f"{path}: listed twice in the lists of sources of aot/" for path in twice]
    for path in sources + headers:
        declarations, left_defined = declarations_in(read(os.path.join(JAVASCRIPTCORE, path)))
        is_source = path.endswith(".cpp")
        for name, line in left_defined.items():
            macros[name].append(f"{path}:{line}")
        for declaration in declarations:
            if declaration.kind == "using namespace":
                if is_source:
                    problems.append(f"{path}:{declaration.line}: `using namespace` reaches every source behind this one in its bundle")
            elif not is_source:
                shared[declaration.name].append((path, declaration))
            elif declaration.is_local:
                own[declaration.name].append((path, declaration))

    def clash(one, other):
        if one.kind == other.kind == "function":
            return one.parameters == other.parameters
        if one.kind == other.kind == "alias":
            return one.text != other.text
        return not (one.kind == other.kind == "type" and not (one.is_defined and other.is_defined))

    def where(path, declaration):
        return f"{path}:{declaration.line}"

    for name, declared in sorted(own.items()):
        for position, (path, declaration) in enumerate(declared):
            for other_path, other in declared[:position]:
                if other_path == path or other.scope != declaration.scope:
                    continue
                if clash(declaration, other):
                    problems.append(f"{where(path, declaration)}: {name} is also what {where(other_path, other)} declares for itself")
                    break
                if declaration.is_unnamed != other.is_unnamed:
                    problems.append(f"{where(path, declaration)}: of {name} here and in {where(other_path, other)}, the one in an unnamed namespace hides the other from callers in an unnamed namespace")
                    break
            for header, other in shared.get(name, ()):
                if declaration.scope[:len(other.scope)] != other.scope:
                    continue
                if len(other.scope) < len(declaration.scope):
                    if not declaration.kind == other.kind == "function":
                        problems.append(f"{where(path, declaration)}: {name} hides what {where(header, other)} declares in a namespace around it")
                        break
                    continue
                if clash(declaration, other) and not (declaration.kind == "type" and not other.is_defined):
                    problems.append(f"{where(path, declaration)}: {name} is declared in {where(header, other)} too")
                    break
    for name, declared in sorted(shared.items()):
        defined = [(path, declaration) for path, declaration in declared if declaration.is_defined and declaration.kind in ("function", "constant")]
        for position, (path, declaration) in enumerate(defined):
            for other_path, other in defined[:position]:
                if other_path != path and other.scope == declaration.scope and clash(declaration, other):
                    problems.append(f"{where(path, declaration)}: {name} is also defined in {where(other_path, other)}")
                    break
    for name, places in sorted(macros.items()):
        if len(places) > 1 and not all(place.split(":")[0].endswith(".h") for place in places):
            problems.append(f"{places[1]}: the macro {name} is still defined at the end of {places[0]} too")

    print(f"{len(sources)} sources and {len(headers)} headers read, {len(problems)} problems")
    for problem in problems:
        print("  " + problem)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
