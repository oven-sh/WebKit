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
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */


#pragma once

#include <wtf/text/ASCIILiteral.h>

namespace JSC { namespace Python {

// sys.stdlib_module_names: Python/stdlib_module_names.h of CPython 3.14, which is made by its Tools/build/generate_stdlib_module_names.py. It is what there is in the library on any system, whether or not it is here.
static constexpr ASCIILiteral standardLibraryModuleNames[] = {
    "__future__"_s, "_abc"_s, "_aix_support"_s, "_android_support"_s, "_apple_support"_s, "_ast"_s, "_ast_unparse"_s, "_asyncio"_s, "_bisect"_s, "_blake2"_s, "_bz2"_s, "_codecs"_s, "_codecs_cn"_s,
    "_codecs_hk"_s, "_codecs_iso2022"_s, "_codecs_jp"_s, "_codecs_kr"_s, "_codecs_tw"_s, "_collections"_s, "_collections_abc"_s, "_colorize"_s, "_compat_pickle"_s, "_contextvars"_s, "_csv"_s,
    "_ctypes"_s, "_curses"_s, "_curses_panel"_s, "_datetime"_s, "_dbm"_s, "_decimal"_s, "_elementtree"_s, "_frozen_importlib"_s, "_frozen_importlib_external"_s, "_functools"_s, "_gdbm"_s,
    "_hashlib"_s, "_heapq"_s, "_hmac"_s, "_imp"_s, "_interpchannels"_s, "_interpqueues"_s, "_interpreters"_s, "_io"_s, "_ios_support"_s, "_json"_s, "_locale"_s, "_lsprof"_s, "_lzma"_s,
    "_markupbase"_s, "_md5"_s, "_multibytecodec"_s, "_multiprocessing"_s, "_opcode"_s, "_opcode_metadata"_s, "_operator"_s, "_osx_support"_s, "_overlapped"_s, "_pickle"_s, "_posixshmem"_s,
    "_posixsubprocess"_s, "_py_abc"_s, "_py_warnings"_s, "_pydatetime"_s, "_pydecimal"_s, "_pyio"_s, "_pylong"_s, "_pyrepl"_s, "_queue"_s, "_random"_s, "_remote_debugging"_s, "_scproxy"_s, "_sha1"_s,
    "_sha2"_s, "_sha3"_s, "_signal"_s, "_sitebuiltins"_s, "_socket"_s, "_sqlite3"_s, "_sre"_s, "_ssl"_s, "_stat"_s, "_statistics"_s, "_string"_s, "_strptime"_s, "_struct"_s, "_suggestions"_s,
    "_symtable"_s, "_sysconfig"_s, "_thread"_s, "_threading_local"_s, "_tkinter"_s, "_tokenize"_s, "_tracemalloc"_s, "_types"_s, "_typing"_s, "_uuid"_s, "_warnings"_s, "_weakref"_s, "_weakrefset"_s,
    "_winapi"_s, "_wmi"_s, "_zoneinfo"_s, "_zstd"_s, "abc"_s, "annotationlib"_s, "antigravity"_s, "argparse"_s, "array"_s, "ast"_s, "asyncio"_s, "atexit"_s, "base64"_s, "bdb"_s, "binascii"_s,
    "bisect"_s, "builtins"_s, "bz2"_s, "cProfile"_s, "calendar"_s, "cmath"_s, "cmd"_s, "code"_s, "codecs"_s, "codeop"_s, "collections"_s, "colorsys"_s, "compileall"_s, "compression"_s, "concurrent"_s,
    "configparser"_s, "contextlib"_s, "contextvars"_s, "copy"_s, "copyreg"_s, "csv"_s, "ctypes"_s, "curses"_s, "dataclasses"_s, "datetime"_s, "dbm"_s, "decimal"_s, "difflib"_s, "dis"_s, "doctest"_s,
    "email"_s, "encodings"_s, "ensurepip"_s, "enum"_s, "errno"_s, "faulthandler"_s, "fcntl"_s, "filecmp"_s, "fileinput"_s, "fnmatch"_s, "fractions"_s, "ftplib"_s, "functools"_s, "gc"_s,
    "genericpath"_s, "getopt"_s, "getpass"_s, "gettext"_s, "glob"_s, "graphlib"_s, "grp"_s, "gzip"_s, "hashlib"_s, "heapq"_s, "hmac"_s, "html"_s, "http"_s, "idlelib"_s, "imaplib"_s, "importlib"_s,
    "inspect"_s, "io"_s, "ipaddress"_s, "itertools"_s, "json"_s, "keyword"_s, "linecache"_s, "locale"_s, "logging"_s, "lzma"_s, "mailbox"_s, "marshal"_s, "math"_s, "mimetypes"_s, "mmap"_s,
    "modulefinder"_s, "msvcrt"_s, "multiprocessing"_s, "netrc"_s, "nt"_s, "ntpath"_s, "nturl2path"_s, "numbers"_s, "opcode"_s, "operator"_s, "optparse"_s, "os"_s, "pathlib"_s, "pdb"_s, "pickle"_s,
    "pickletools"_s, "pkgutil"_s, "platform"_s, "plistlib"_s, "poplib"_s, "posix"_s, "posixpath"_s, "pprint"_s, "profile"_s, "pstats"_s, "pty"_s, "pwd"_s, "py_compile"_s, "pyclbr"_s, "pydoc"_s,
    "pydoc_data"_s, "pyexpat"_s, "queue"_s, "quopri"_s, "random"_s, "re"_s, "readline"_s, "reprlib"_s, "resource"_s, "rlcompleter"_s, "runpy"_s, "sched"_s, "secrets"_s, "select"_s, "selectors"_s,
    "shelve"_s, "shlex"_s, "shutil"_s, "signal"_s, "site"_s, "smtplib"_s, "socket"_s, "socketserver"_s, "sqlite3"_s, "sre_compile"_s, "sre_constants"_s, "sre_parse"_s, "ssl"_s, "stat"_s,
    "statistics"_s, "string"_s, "stringprep"_s, "struct"_s, "subprocess"_s, "symtable"_s, "sys"_s, "sysconfig"_s, "syslog"_s, "tabnanny"_s, "tarfile"_s, "tempfile"_s, "termios"_s, "textwrap"_s,
    "this"_s, "threading"_s, "time"_s, "timeit"_s, "tkinter"_s, "token"_s, "tokenize"_s, "tomllib"_s, "trace"_s, "traceback"_s, "tracemalloc"_s, "tty"_s, "turtle"_s, "turtledemo"_s, "types"_s,
    "typing"_s, "unicodedata"_s, "unittest"_s, "urllib"_s, "uuid"_s, "venv"_s, "warnings"_s, "wave"_s, "weakref"_s, "webbrowser"_s, "winreg"_s, "winsound"_s, "wsgiref"_s, "xml"_s, "xmlrpc"_s,
    "zipapp"_s, "zipfile"_s, "zipimport"_s, "zlib"_s, "zoneinfo"_s,
};

} } // namespace JSC::Python
