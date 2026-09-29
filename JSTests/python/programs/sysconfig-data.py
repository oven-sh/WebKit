# What sysconfig is told about how the interpreter was built. There is far less of it here than in CPython, most of which is about the C compiler. What there is says what CPython says.
import importlib
import sys
import sysconfig

name = sysconfig._get_sysconfigdata_name()
print(name, sys.implementation._multiarch, sys.platform, repr(sys.abiflags))
data = importlib.import_module(name)
print(type(data.build_time_vars).__name__, data.__name__ == name)
for key in ("ABIFLAGS", "EXE", "LDVERSION", "MACHDEP", "MULTIARCH", "PLATLIBDIR", "PYTHONFRAMEWORK", "Py_DEBUG", "Py_ENABLE_SHARED", "Py_GIL_DISABLED", "Py_TRACE_REFS", "TZPATH", "VERSION", "WITH_DOC_STRINGS"):
    print(key, repr(data.build_time_vars[key]), repr(sysconfig.get_config_var(key)))
print(sysconfig.get_config_var("there is no such thing"), sysconfig.get_config_vars("VERSION", "nor this"))
print(sysconfig.get_python_version(), sysconfig.get_default_scheme(), sysconfig.get_preferred_scheme("prefix"), sorted(sysconfig.get_path_names()), sorted(sysconfig.get_scheme_names()))
print(sysconfig.get_platform().split("-")[0], sysconfig.get_platform().split("-")[-1], sysconfig.is_python_build())
for key in ("py_version", "py_version_short", "py_version_nodot", "abiflags", "platlibdir", "abi_thread", "implementation", "implementation_lower"):
    print(key, repr(sysconfig.get_config_var(key)))
