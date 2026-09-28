# FrameLocalsProxy: what frame.f_locals is for the frame of a function. It reads and writes the variables where they are (PEP 667).
#
# It follows Objects/frameobject.c of CPython, function for function.

from _frame import frame as _frame_type, variable_names, get_variable, set_variable, extra_locals

_unbound = object()
_being_shown = []


# Which variable of the frame a key is the name of, or -1. To be read it has to be bound.
def _index(frame, key, read):
    hash(key)
    for i, name in enumerate(variable_names(frame)):
        if name == key and (not read or get_variable(frame, i, _unbound) is not _unbound):
            return i
    return -1


def _is_dict_or_proxy(value):
    return isinstance(value, (dict, FrameLocalsProxy))


class FrameLocalsProxy:
    __module__ = "builtins"
    __slots__ = ("_frame",)
    __hash__ = None

    def __new__(cls, *args, **kwargs):
        if len(args) != 1:
            raise TypeError("FrameLocalsProxy expected 1 argument, got %d" % len(args))
        if not isinstance(args[0], _frame_type):
            raise TypeError("expect frame, not %s" % type(args[0]).__name__)
        if kwargs:
            raise TypeError("FrameLocalsProxy takes no keyword arguments")
        self = object.__new__(cls)
        self._frame = args[0]
        return self

    def __getitem__(self, key, /):
        frame = self._frame
        i = _index(frame, key, True)
        if i >= 0:
            return get_variable(frame, i, None)
        extra = extra_locals(frame, False)
        if extra is not None and key in extra:
            return extra[key]
        raise KeyError("local variable '%r' is not defined" % (key,))

    def __setitem__(self, key, value, /):
        frame = self._frame
        i = _index(frame, key, False)
        if i >= 0:
            set_variable(frame, i, value)
        else:
            extra_locals(frame, True)[key] = value

    def __delitem__(self, key, /):
        frame = self._frame
        if _index(frame, key, False) >= 0:
            raise ValueError("cannot remove local variables from FrameLocalsProxy")
        extra = extra_locals(frame, False)
        if extra is None:
            raise KeyError(key)
        del extra[key]

    def _merge(self, other):
        if not _is_dict_or_proxy(other):
            return False
        for key in other.keys():
            self[key] = other[key]
        return True

    def keys(self):
        frame = self._frame
        names = [name for i, name in enumerate(variable_names(frame)) if get_variable(frame, i, _unbound) is not _unbound]
        extra = extra_locals(frame, False)
        if extra is not None:
            names.extend(extra)
        return names

    def values(self):
        frame = self._frame
        values = []
        for i in range(len(variable_names(frame))):
            value = get_variable(frame, i, _unbound)
            if value is not _unbound:
                values.append(value)
        extra = extra_locals(frame, False)
        if extra is not None:
            values.extend(extra.values())
        return values

    def items(self):
        frame = self._frame
        items = []
        for i, name in enumerate(variable_names(frame)):
            value = get_variable(frame, i, _unbound)
            if value is not _unbound:
                items.append((name, value))
        extra = extra_locals(frame, False)
        if extra is not None:
            items.extend(extra.items())
        return items

    def __iter__(self):
        return iter(self.keys())

    def __reversed__(self):
        names = self.keys()
        names.reverse()
        return names

    def __len__(self):
        return len(self.keys())

    def __contains__(self, key, /):
        frame = self._frame
        if _index(frame, key, True) >= 0:
            return True
        extra = extra_locals(frame, False)
        return extra is not None and key in extra

    def __eq__(self, other):
        if isinstance(other, FrameLocalsProxy):
            return self._frame is other._frame
        if isinstance(other, dict):
            return dict(self) == other
        return NotImplemented

    def __ne__(self, other):
        if isinstance(other, FrameLocalsProxy):
            return self._frame is not other._frame
        if isinstance(other, dict):
            return dict(self) != other
        return NotImplemented

    def __lt__(self, other):
        return dict(self) < other if isinstance(other, dict) else NotImplemented

    def __le__(self, other):
        return dict(self) <= other if isinstance(other, dict) else NotImplemented

    def __gt__(self, other):
        return dict(self) > other if isinstance(other, dict) else NotImplemented

    def __ge__(self, other):
        return dict(self) >= other if isinstance(other, dict) else NotImplemented

    def __repr__(self):
        for shown in _being_shown:
            if shown is self:
                return "{...}"
        _being_shown.append(self)
        try:
            return repr(dict(self))
        finally:
            _being_shown.pop()

    def __or__(self, other):
        if not _is_dict_or_proxy(other):
            return NotImplemented
        result = dict(self)
        result.update(other)
        return result

    def __ror__(self, other):
        if not _is_dict_or_proxy(other):
            return NotImplemented
        result = dict(other)
        result.update(self)
        return result

    def __ior__(self, other):
        if not self._merge(other):
            return NotImplemented
        return self

    def update(self, other, /):
        if not self._merge(other):
            raise TypeError("update() argument must be dict or another FrameLocalsProxy")

    def get(self, *args):
        if not 1 <= len(args) <= 2:
            raise TypeError("get expected 1 or 2 arguments")
        try:
            return self[args[0]]
        except KeyError:
            return args[1] if len(args) == 2 else None

    def setdefault(self, *args):
        if not 1 <= len(args) <= 2:
            raise TypeError("setdefault expected 1 or 2 arguments")
        try:
            return self[args[0]]
        except KeyError:
            value = args[1] if len(args) == 2 else None
            self[args[0]] = value
            return value

    def pop(self, *args):
        if len(args) < 1:
            raise TypeError("pop expected at least 1 argument, got 0")
        if len(args) > 2:
            raise TypeError("pop expected at most 2 arguments, got %d" % len(args))
        key = args[0]
        frame = self._frame
        if _index(frame, key, False) >= 0:
            raise ValueError("cannot remove local variables from FrameLocalsProxy")
        extra = extra_locals(frame, False)
        if extra is not None and key in extra:
            return extra.pop(key)
        if len(args) == 2:
            return args[1]
        raise KeyError(key)

    def copy(self):
        return dict(self)
