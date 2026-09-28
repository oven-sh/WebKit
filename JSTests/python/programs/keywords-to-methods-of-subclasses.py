def attempt(f):
    try: return f()
    except BaseException as e: return type(e).__name__ + ": " + str(e)
class L(list): pass
class D(dict): pass
class S(str): pass
class E(ValueError): pass
print(attempt(lambda: L().append(x=1)))
print(attempt(lambda: L().append()))
print(attempt(lambda: L().extend(x=1)))
print(attempt(lambda: L().clear(x=1)))
print(attempt(lambda: L().pop(x=1)))
print(attempt(lambda: D().get(x=1)))
print(attempt(lambda: D().keys(x=1)))
print(attempt(lambda: S().upper(x=1)))
print(attempt(lambda: S().startswith(x=1)))
print(attempt(lambda: E().add_note(x=1)))
print(attempt(lambda: E().with_traceback(x=1)))
print(attempt(lambda: L.append(L(), x=1)))
print(attempt(lambda: list.append(L(), x=1)))
