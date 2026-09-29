# JavaScriptImporter: what finds and loads a module that is JavaScript's. It is the last on sys.meta_path, so what is Python's by a name is what the name means.
#
# Where to look is for whoever embeds the engine to say (Configuration::findJavaScriptModule). What is loaded is the module's namespace object, as it is, which is a module to Python.

from _frozen_importlib import ModuleSpec
from _javascript import find_module, load_module
import sys


class JavaScriptImporter:
    @classmethod
    def find_spec(cls, fullname, path=None, target=None):
        # A name that is the standard library's is Python's, whether or not there is such a module here. What tries `import readline` to see whether it can is not to be given something else.
        if fullname.partition(".")[0] in sys.stdlib_module_names:
            return None
        found = find_module(fullname, sys.path if path is None else path)
        if found is None:
            return None
        key, file, package = found
        # With nothing to load, it is only somewhere for others to be: a namespace package.
        spec = ModuleSpec(fullname, None if key is None else cls, origin=key if file is None else file, loader_state=key, is_package=package is not None)
        spec.has_location = file is not None
        if package is not None:
            spec.submodule_search_locations.append(package)
        return spec

    # It is run as it is loaded. If it imports what imports it, that is given the same object, with as much in it as there is by then.
    @staticmethod
    def create_module(spec):
        module = load_module(spec.loader_state)
        # As module.__init__() leaves it. Otherwise it is that of the class that is found.
        vars(module).setdefault("__doc__", None)
        return module

    @staticmethod
    def exec_module(module):
        pass
