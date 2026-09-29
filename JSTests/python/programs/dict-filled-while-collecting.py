# The collector goes through a dict or a set on another thread while it is being filled. What it reads is to be what was there or what is there now, and not half of each, which is so only of what is aligned.
# With the keys and values four bytes out, this crashed one time in three, in the collector, with the low half of an address for an address.


def f(**k):
    return len(k)


d = {"k%d" % i: i for i in range(70000)}
total = 0
for again in range(60):
    total += f(**d)
    total += len(set(d))
print(total)
