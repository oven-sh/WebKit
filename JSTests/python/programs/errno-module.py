# The module errno
import errno

print(errno.__name__, errno.__package__, errno.__loader__.__name__, errno.__spec__.origin)
print(errno.__doc__)
print([(name, getattr(errno, name)) for name in vars(errno) if not name.startswith("__") and name != "errorcode"])
print(list(errno.errorcode.items()))
print(type(errno.errorcode).__name__, all(getattr(errno, name) == code for code, name in errno.errorcode.items()))
