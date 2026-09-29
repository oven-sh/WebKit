# What `from __future__ import feature` binds the names to: for each feature that there has been, when it could first be asked for, when it no longer had to be, and the flag that compile() takes for it. The statement
# is for the compiler, which has done what it asks by the time that this is run, and is an import besides. It is here, with the compiler, because the two have to agree.

all_feature_names = ["nested_scopes", "generators", "division", "absolute_import", "with_statement", "print_function", "unicode_literals", "barry_as_FLUFL", "generator_stop", "annotations"]

__all__ = ["all_feature_names"] + all_feature_names

# By the names that CPython's code.h has for them.
CO_NESTED = 0x0010
CO_GENERATOR_ALLOWED = 0
CO_FUTURE_DIVISION = 0x20000
CO_FUTURE_ABSOLUTE_IMPORT = 0x40000
CO_FUTURE_WITH_STATEMENT = 0x80000
CO_FUTURE_PRINT_FUNCTION = 0x100000
CO_FUTURE_UNICODE_LITERALS = 0x200000
CO_FUTURE_BARRY_AS_BDFL = 0x400000
CO_FUTURE_GENERATOR_STOP = 0x800000
CO_FUTURE_ANNOTATIONS = 0x1000000


class _Feature:
    def __init__(self, optionalRelease, mandatoryRelease, compiler_flag):
        self.optional = optionalRelease
        self.mandatory = mandatoryRelease
        self.compiler_flag = compiler_flag

    def getOptionalRelease(self):
        """Return first release in which this feature was recognized.

        This is a 5-tuple, of the same form as sys.version_info.
        """
        return self.optional

    def getMandatoryRelease(self):
        """Return release in which this feature will become mandatory.

        This is a 5-tuple, of the same form as sys.version_info, or, if
        the feature was dropped, or the release date is undetermined, is None.
        """
        return self.mandatory

    def __repr__(self):
        return "_Feature" + repr((self.optional, self.mandatory, self.compiler_flag))


nested_scopes = _Feature((2, 1, 0, "beta", 1), (2, 2, 0, "alpha", 0), CO_NESTED)
generators = _Feature((2, 2, 0, "alpha", 1), (2, 3, 0, "final", 0), CO_GENERATOR_ALLOWED)
division = _Feature((2, 2, 0, "alpha", 2), (3, 0, 0, "alpha", 0), CO_FUTURE_DIVISION)
absolute_import = _Feature((2, 5, 0, "alpha", 1), (3, 0, 0, "alpha", 0), CO_FUTURE_ABSOLUTE_IMPORT)
with_statement = _Feature((2, 5, 0, "alpha", 1), (2, 6, 0, "alpha", 0), CO_FUTURE_WITH_STATEMENT)
print_function = _Feature((2, 6, 0, "alpha", 2), (3, 0, 0, "alpha", 0), CO_FUTURE_PRINT_FUNCTION)
unicode_literals = _Feature((2, 6, 0, "alpha", 2), (3, 0, 0, "alpha", 0), CO_FUTURE_UNICODE_LITERALS)
barry_as_FLUFL = _Feature((3, 1, 0, "alpha", 2), (4, 0, 0, "alpha", 0), CO_FUTURE_BARRY_AS_BDFL)
generator_stop = _Feature((3, 5, 0, "beta", 1), (3, 7, 0, "alpha", 0), CO_FUTURE_GENERATOR_STOP)
annotations = _Feature((3, 7, 0, "beta", 1), None, CO_FUTURE_ANNOTATIONS)
