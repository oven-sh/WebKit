# BaseExceptionGroup and ExceptionGroup, and what `except*` is compiled into calls of.
#
# It follows Objects/exceptions.c of CPython, function for function.


class BaseExceptionGroup(BaseException):
    """A combination of multiple unrelated exceptions."""

    __module__ = "builtins"
    __slots__ = ("_message", "_exceptions", "_exceptions_repr")

    def __new__(cls, *args, **kwargs):
        if len(args) != 2:
            raise TypeError("BaseExceptionGroup.__new__() takes exactly 2 arguments (%d given)" % len(args))
        message, exceptions = args
        if not isinstance(message, str):
            raise TypeError("BaseExceptionGroup.__new__() argument 1 must be str, not %s"
                            % ("None" if message is None else type(message).__name__))
        if isinstance(exceptions, dict) or not hasattr(type(exceptions), "__getitem__"):
            raise TypeError("second argument (exceptions) must be a sequence")

        # What it looked like, in case it is something that changes.
        exceptions_repr = None
        if not isinstance(exceptions, (list, tuple)):
            exceptions_repr = repr(exceptions)
        exceptions = tuple(exceptions)
        if not exceptions:
            raise ValueError("second argument (exceptions) must be a non-empty sequence")

        nested_base_exceptions = False
        for i, exc in enumerate(exceptions):
            if not isinstance(exc, BaseException):
                raise ValueError("Item %d of second argument (exceptions) is not an exception" % i)
            if not isinstance(exc, Exception):
                nested_base_exceptions = True

        if cls is ExceptionGroup:
            if nested_base_exceptions:
                raise TypeError("Cannot nest BaseExceptions in an ExceptionGroup")
        elif cls is BaseExceptionGroup:
            if not nested_base_exceptions:
                cls = ExceptionGroup
        elif nested_base_exceptions and issubclass(cls, Exception):
            raise TypeError("Cannot nest BaseExceptions in '%s'" % cls.__name__)

        self = BaseException.__new__(cls, *args)
        self._message = message
        self._exceptions = exceptions
        self._exceptions_repr = exceptions_repr
        return self

    def __init__(self, *args, **kwargs):
        if kwargs:
            raise TypeError("%s() takes no keyword arguments" % type(self).__name__)
        BaseException.__init__(self, *args)

    message = property(lambda self: self._message, doc="exception message")
    exceptions = property(lambda self: self._exceptions, doc="nested exceptions")

    def __str__(self):
        count = len(self._exceptions)
        return "%s (%d sub-exception%s)" % (self._message, count, "s" if count > 1 else "")

    def __repr__(self):
        if self._exceptions_repr is not None:
            exceptions = self._exceptions_repr
        elif isinstance(self.args[1], list):
            exceptions = repr(list(self._exceptions))
        else:
            exceptions = repr(self._exceptions)
        return "%s(%r, %s)" % (type(self).__name__, self._message, exceptions)

    def derive(self, excs, /):
        return BaseExceptionGroup(self._message, excs)

    def split(self, matcher_value, /):
        return _split(self, _matcher(matcher_value), True)

    def subgroup(self, matcher_value, /):
        return _split(self, _matcher(matcher_value), False)[0]


class ExceptionGroup(BaseExceptionGroup, Exception):
    __module__ = "builtins"
    __slots__ = ()


def _is_exception_class(value):
    return isinstance(value, type) and issubclass(value, BaseException)


# A function that says whether an exception matches, from what split() and subgroup() are given.
def _matcher(value):
    if callable(value) and not isinstance(value, type):
        return lambda exc: bool(value(exc))
    if _is_exception_class(value) or (type(value) is tuple and all(_is_exception_class(item) for item in value)):
        return lambda exc: isinstance(exc, value)
    raise TypeError("expected an exception type, a tuple of exception types, or a callable (other than a class)")


# A group like `orig` with only `excs` in it, or None if there are none.
def _subset(orig, excs):
    if not excs:
        return None
    group = orig.derive(excs)
    if not isinstance(group, BaseExceptionGroup):
        raise TypeError("derive must return an instance of BaseExceptionGroup")
    traceback = orig.__traceback__
    if traceback is not None:
        group.__traceback__ = traceback
    group.__context__ = orig.__context__
    suppress = group.__suppress_context__
    group.__cause__ = orig.__cause__
    group.__suppress_context__ = suppress
    notes = getattr(orig, "__notes__", None)
    if notes is not None and hasattr(type(notes), "__getitem__") and not isinstance(notes, dict):
        group.__notes__ = list(notes)
    return group


# (what matches, the rest). The rest is only worked out if it is wanted.
def _split(exc, matches, construct_rest):
    if matches(exc):
        return exc, None
    if not isinstance(exc, BaseExceptionGroup):
        return None, (exc if construct_rest else None)
    match_list = []
    rest_list = []
    for item in exc._exceptions:
        match, rest = _split(item, matches, construct_rest)
        if match is not None:
            match_list.append(match)
        if rest is not None:
            rest_list.append(rest)
    return _subset(exc, match_list), (_subset(exc, rest_list) if construct_rest else None)


def _check_star_type(match_type):
    for item in match_type if isinstance(match_type, tuple) else (match_type,):
        if not _is_exception_class(item):
            raise TypeError("catching classes that do not inherit from BaseException is not allowed")
        if issubclass(item, BaseExceptionGroup):
            raise TypeError("catching ExceptionGroup with except* is not allowed. Use except instead.")


# except* match_type: (what of the exception the clause handles, what is left for the clauses after it). Either can be None.
def match_exception_group(exc, match_type):
    _check_star_type(match_type)
    if exc is None:
        return None, None
    if isinstance(exc, match_type):
        if isinstance(exc, BaseExceptionGroup):
            return exc, None
        # A bare exception is handled as a group of one.
        wrapped = BaseExceptionGroup("", (exc,))
        wrapped.__traceback__ = exc.__traceback__
        return wrapped, None
    if isinstance(exc, BaseExceptionGroup):
        return exc.split(match_type)
    return None, exc


def _collect_leaves(exc, leaves):
    if exc is None:
        return
    if not isinstance(exc, BaseExceptionGroup):
        leaves.add(id(exc))
        return
    for item in exc._exceptions:
        _collect_leaves(item, leaves)


def _same_metadata(a, b):
    return (getattr(a, "__notes__", None) is getattr(b, "__notes__", None)
            and a.__traceback__ is b.__traceback__
            and a.__cause__ is b.__cause__
            and a.__context__ is b.__context__)


# What to raise when a try with except* clauses is done, if anything. `orig` is what was caught, and `excs` is what the clauses raised,
# or raised again, and what none of them handled.
def prepare_reraise_star(orig, excs):
    if not excs:
        return None
    if not isinstance(orig, BaseExceptionGroup):
        # It was wrapped for the one clause that can have handled it.
        return excs[0]
    raised = []
    reraised = []
    for exc in excs:
        if exc is not None:
            (reraised if _same_metadata(exc, orig) else raised).append(exc)

    # What was raised again keeps the place that it had in the original.
    leaves = set()
    for exc in reraised:
        _collect_leaves(exc, leaves)
    reraised_group = _split(orig, lambda exc: not isinstance(exc, BaseExceptionGroup) and id(exc) in leaves, False)[0]

    if not raised:
        return reraised_group
    if reraised_group is not None:
        raised.append(reraised_group)
    if len(raised) > 1:
        return BaseExceptionGroup("", raised)
    return raised[0]
