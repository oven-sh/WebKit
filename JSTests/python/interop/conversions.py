class Num:
    def __float__(self): return 7.5
    def __index__(self): return 7
    def __str__(self): return "numstr"
class Idx:
    def __index__(self): return 3
class Big:
    def __index__(self): return 2**70
class OnlyInt:
    def __int__(self): return 9
class Plain:
    def __str__(self): return "plain"
class MyInt(int): pass
class MyFloat(float): pass
class MyStr(str): pass
class Bad:
    def __float__(self): return "no"
z = 1 + 2j
t = (1, 2)
