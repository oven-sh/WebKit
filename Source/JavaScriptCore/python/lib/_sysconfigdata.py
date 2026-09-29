# What `sysconfig` is told about how the interpreter was built. CPython writes a file like this when it is built, with some hundreds of entries, nearly all of them about the C compiler, what the C library was found to have,
# and where things were installed. What is here is what is true of this implementation. What is asked for and is not here is None, which is what `sysconfig` says of what it has not heard of.
#
# There is nothing about extension modules (EXT_SUFFIX, SOABI, CC, CFLAGS, LDSHARED), there being none.
import sys

build_time_vars = {
    "ABIFLAGS": sys.abiflags,
    "EXE": "",
    "LDVERSION": "%d.%d%s" % (sys.version_info[0], sys.version_info[1], sys.abiflags),
    "MACHDEP": sys.platform,
    "MULTIARCH": sys.implementation._multiarch,
    "PLATLIBDIR": sys.platlibdir,
    "PYTHONFRAMEWORK": "",
    "Py_DEBUG": 0,
    "Py_ENABLE_SHARED": 0,
    "Py_GIL_DISABLED": 0,
    "Py_TRACE_REFS": 0,
    # Where `zoneinfo` looks for time zones
    "TZPATH": "/usr/share/zoneinfo:/usr/lib/zoneinfo:/usr/share/lib/zoneinfo:/etc/zoneinfo",
    "VERSION": "%d.%d" % sys.version_info[:2],
    "WITH_DOC_STRINGS": 1,
}
