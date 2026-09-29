// Made by lib/convert-usage-text.py out of Python/initconfig.c of CPython. It is not to be changed by hand.

static const char usage_line[] =
"usage: %s [option] ... [-c cmd | -m mod | file | -] [arg] ...\n";

/* Long help message */
/* Lines sorted by option name; keep in sync with usage_envvars* below */
static const char usage_help[] = "\
Options (and corresponding environment variables):\n\
-b     : issue warnings about converting bytes/bytearray to str and comparing\n\
         bytes/bytearray with str or bytes with int. (-bb: issue errors)\n\
-B     : don't write .pyc files on import; also PYTHONDONTWRITEBYTECODE=x\n\
-c cmd : program passed in as string (terminates option list)\n\
-d     : turn on parser debugging output (for experts only, only works on\n\
         debug builds); also PYTHONDEBUG=x\n\
-E     : ignore PYTHON* environment variables (such as PYTHONPATH)\n\
-h     : print this help message and exit (also -? or --help)\n\
-i     : inspect interactively after running script; forces a prompt even\n\
         if stdin does not appear to be a terminal; also PYTHONINSPECT=x\n\
-I     : isolate Python from the user's environment (implies -E, -P and -s)\n\
-m mod : run library module as a script (terminates option list)\n\
-O     : remove assert and __debug__-dependent statements; add .opt-1 before\n\
         .pyc extension; also PYTHONOPTIMIZE=x\n\
-OO    : do -O changes and also discard docstrings; add .opt-2 before\n\
         .pyc extension\n\
-P     : don't prepend a potentially unsafe path to sys.path; also\n\
         PYTHONSAFEPATH\n\
-q     : don't print version and copyright messages on interactive startup\n\
-s     : don't add user site directory to sys.path; also PYTHONNOUSERSITE=x\n\
-S     : don't imply 'import site' on initialization\n\
-u     : force the stdout and stderr streams to be unbuffered;\n\
         this option has no effect on stdin; also PYTHONUNBUFFERED=x\n\
-v     : verbose (trace import statements); also PYTHONVERBOSE=x\n\
         can be supplied multiple times to increase verbosity\n\
-V     : print the Python version number and exit (also --version)\n\
         when given twice, print more information about the build\n\
-W arg : warning control; arg is action:message:category:module:lineno\n\
         also PYTHONWARNINGS=arg\n\
-x     : skip first line of source, allowing use of non-Unix forms of #!cmd\n\
-X opt : set implementation-specific option\n\
--check-hash-based-pycs always|default|never:\n\
         control how Python invalidates hash-based .pyc files\n\
--help-env: print help about Python environment variables and exit\n\
--help-xoptions: print help about implementation-specific -X options and exit\n\
--help-all: print complete help information and exit\n\
\n\
Arguments:\n\
file   : program read from script file\n\
-      : program read from stdin (default; interactive mode if a tty)\n\
arg ...: arguments passed to program in sys.argv[1:]\n\
";

static const char usage_xoptions[] = "\
The following implementation-specific options are available:\n\
-X context_aware_warnings=[0|1]: if true (1) then the warnings module will\n\
         use a context variables; if false (0) then the warnings module will\n\
         use module globals, which is not concurrent-safe; set to true for\n\
         free-threaded builds and false otherwise; also\n\
         PYTHON_CONTEXT_AWARE_WARNINGS\n\
-X cpu_count=N: override the return value of os.cpu_count();\n\
         -X cpu_count=default cancels overriding; also PYTHON_CPU_COUNT\n\
-X dev : enable Python Development Mode; also PYTHONDEVMODE\n\
-X disable-remote-debug: disable remote debugging; also PYTHON_DISABLE_REMOTE_DEBUG\n\
-X faulthandler: dump the Python traceback on fatal errors;\n\
         also PYTHONFAULTHANDLER\n\
-X frozen_modules=[on|off]: whether to use frozen modules; the default is \"on\"\n\
         for installed Python and \"off\" for a local build;\n\
         also PYTHON_FROZEN_MODULES\n\
"
#ifdef Py_GIL_DISABLED
"-X gil=[0|1]: enable (1) or disable (0) the GIL; also PYTHON_GIL\n"
#endif
"\
-X importtime[=2]: show how long each import takes; use -X importtime=2 to\n\
         log imports of already-loaded modules; also PYTHONPROFILEIMPORTTIME\n\
-X int_max_str_digits=N: limit the size of int<->str conversions;\n\
         0 disables the limit; also PYTHONINTMAXSTRDIGITS\n\
-X no_debug_ranges: don't include extra location information in code objects;\n\
         also PYTHONNODEBUGRANGES\n\
-X perf: support the Linux \"perf\" profiler; also PYTHONPERFSUPPORT=1\n\
-X perf_jit: support the Linux \"perf\" profiler with DWARF support;\n\
         also PYTHON_PERF_JIT_SUPPORT=1\n\
"
#ifdef Py_DEBUG
"-X presite=MOD: import this module before site; also PYTHON_PRESITE\n"
#endif
"\
-X pycache_prefix=PATH: write .pyc files to a parallel tree instead of to the\n\
         code tree; also PYTHONPYCACHEPREFIX\n\
"
#ifdef Py_STATS
"-X pystats: enable pystats collection at startup; also PYTHONSTATS\n"
#endif
"\
-X showrefcount: output the total reference count and number of used\n\
         memory blocks when the program finishes or after each statement in\n\
         the interactive interpreter; only works on debug builds\n\
-X thread_inherit_context=[0|1]: enable (1) or disable (0) threads inheriting\n\
         context vars by default; enabled by default in the free-threaded\n\
         build and disabled otherwise; also PYTHON_THREAD_INHERIT_CONTEXT\n\
"
#ifdef Py_GIL_DISABLED
"-X tlbc=[0|1]: enable (1) or disable (0) thread-local bytecode. Also\n\
         PYTHON_TLBC\n"
#endif
"\
-X tracemalloc[=N]: trace Python memory allocations; N sets a traceback limit\n\
         of N frames (default: 1); also PYTHONTRACEMALLOC=N\n\
-X utf8[=0|1]: enable (1) or disable (0) UTF-8 mode; also PYTHONUTF8\n\
-X warn_default_encoding: enable opt-in EncodingWarning for 'encoding=None';\n\
         also PYTHONWARNDEFAULTENCODING\
";

/* Envvars that don't have equivalent command-line options are listed first */
static const char usage_envvars[] =
"Environment variables that change behavior:\n"
"PYTHONASYNCIODEBUG: enable asyncio debug mode\n"
"PYTHON_BASIC_REPL: use the traditional parser-based REPL\n"
"PYTHONBREAKPOINT: if this variable is set to 0, it disables the default\n"
"                  debugger.  It can be set to the callable of your debugger of\n"
"                  choice.\n"
"PYTHONCASEOK    : ignore case in 'import' statements (Windows)\n"
"PYTHONCOERCECLOCALE: if this variable is set to 0, it disables the locale\n"
"                  coercion behavior.  Use PYTHONCOERCECLOCALE=warn to request\n"
"                  display of locale coercion and locale compatibility warnings\n"
"                  on stderr.\n"
"PYTHON_COLORS   : if this variable is set to 1, the interpreter will colorize\n"
"                  various kinds of output.  Setting it to 0 deactivates\n"
"                  this behavior.\n"
#ifdef Py_TRACE_REFS
"PYTHONDUMPREFS  : dump objects and reference counts still alive after shutdown\n"
"PYTHONDUMPREFSFILE: dump objects and reference counts to the specified file\n"
#endif
#ifdef __APPLE__
"PYTHONEXECUTABLE: set sys.argv[0] to this value (macOS only)\n"
#endif
"PYTHONHASHSEED  : if this variable is set to 'random', a random value is used\n"
"                  to seed the hashes of str and bytes objects.  It can also be\n"
"                  set to an integer in the range [0,4294967295] to get hash\n"
"                  values with a predictable seed.\n"
"PYTHON_HISTORY  : the location of a .python_history file.\n"
"PYTHONHOME      : alternate <prefix> directory (or <prefix>%c<exec_prefix>).\n"
"                  The default module search path uses %s.\n"
"PYTHONIOENCODING: encoding[:errors] used for stdin/stdout/stderr\n"
#ifdef MS_WINDOWS
"PYTHONLEGACYWINDOWSFSENCODING: use legacy \"mbcs\" encoding for file system\n"
"PYTHONLEGACYWINDOWSSTDIO: use legacy Windows stdio\n"
#endif
"PYTHONMALLOC    : set the Python memory allocators and/or install debug hooks\n"
"                  on Python memory allocators.  Use PYTHONMALLOC=debug to\n"
"                  install debug hooks.\n"
"PYTHONMALLOCSTATS: print memory allocator statistics\n"
"PYTHONPATH      : '%c'-separated list of directories prefixed to the\n"
"                  default module search path.  The result is sys.path.\n"
"PYTHONPLATLIBDIR: override sys.platlibdir\n"
"PYTHONSTARTUP   : file executed on interactive startup (no default)\n"
"PYTHONUSERBASE  : defines the user base directory (site.USER_BASE)\n"
"\n"
"These variables have equivalent command-line options (see --help for details):\n"
"PYTHON_CONTEXT_AWARE_WARNINGS: if true (1), enable thread-safe warnings\n"
"                  module behaviour (-X context_aware_warnings)\n"
"PYTHON_CPU_COUNT: override the return value of os.cpu_count() (-X cpu_count)\n"
"PYTHONDEBUG     : enable parser debug mode (-d)\n"
"PYTHONDEVMODE   : enable Python Development Mode (-X dev)\n"
"PYTHONDONTWRITEBYTECODE: don't write .pyc files (-B)\n"
"PYTHONFAULTHANDLER: dump the Python traceback on fatal errors (-X faulthandler)\n"
"PYTHON_FROZEN_MODULES: whether to use frozen modules; the default is \"on\"\n"
"                  for installed Python and \"off\" for a local build\n"
"                  (-X frozen_modules)\n"
#ifdef Py_GIL_DISABLED
"PYTHON_GIL      : when set to 0, disables the GIL (-X gil)\n"
#endif
"PYTHONINSPECT   : inspect interactively after running script (-i)\n"
"PYTHONINTMAXSTRDIGITS: limit the size of int<->str conversions;\n"
"                  0 disables the limit (-X int_max_str_digits=N)\n"
"PYTHONNODEBUGRANGES: don't include extra location information in code objects\n"
"                  (-X no_debug_ranges)\n"
"PYTHONNOUSERSITE: disable user site directory (-s)\n"
"PYTHONOPTIMIZE  : enable level 1 optimizations (-O)\n"
"PYTHON_PERF_JIT_SUPPORT: enable Linux \"perf\" profiler support with JIT\n"
"                  (-X perf_jit)\n"
"PYTHONPERFSUPPORT: support the Linux \"perf\" profiler (-X perf)\n"
#ifdef Py_DEBUG
"PYTHON_PRESITE: import this module before site (-X presite)\n"
#endif
"PYTHONPROFILEIMPORTTIME: show how long each import takes (-X importtime)\n"
"PYTHONPYCACHEPREFIX: root directory for bytecode cache (pyc) files\n"
"                  (-X pycache_prefix)\n"
"PYTHONSAFEPATH  : don't prepend a potentially unsafe path to sys.path.\n"
#ifdef Py_STATS
"PYTHONSTATS     : turns on statistics gathering (-X pystats)\n"
#endif
"PYTHON_THREAD_INHERIT_CONTEXT: if true (1), threads inherit context vars\n"
"                  (-X thread_inherit_context)\n"
#ifdef Py_GIL_DISABLED
"PYTHON_TLBC     : when set to 0, disables thread-local bytecode (-X tlbc)\n"
#endif
"PYTHONTRACEMALLOC: trace Python memory allocations (-X tracemalloc)\n"
"PYTHONUNBUFFERED: disable stdout/stderr buffering (-u)\n"
"PYTHONUTF8      : control the UTF-8 mode (-X utf8)\n"
"PYTHONVERBOSE   : trace import statements (-v)\n"
"PYTHONWARNDEFAULTENCODING: enable opt-in EncodingWarning for 'encoding=None'\n"
"                  (-X warn_default_encoding)\n"
"PYTHONWARNINGS  : warning control (-W)\n"
;

#if defined(MS_WINDOWS)
#  define PYTHONHOMEHELP "<prefix>\\python{major}{minor}"
#else
#  define PYTHONHOMEHELP "<prefix>/lib/pythonX.X"
#endif
