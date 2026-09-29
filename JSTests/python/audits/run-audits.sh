#!/bin/sh
#
# run-audits.sh <path to jsc> [how many differences to show]
#
# These are not tests, which pass or fail. They measure how far what is built in is from what CPython has, by going through all of it:
#
#   own-attributes.py    what each built-in class has in its __dict__, and what kind of thing each is
#   wrong-arguments.py   what is said when each method of each built-in class, and each built-in function, is called with no arguments, with nine, with
#                        a keyword that it does not have, with no instance, and with an instance of the wrong class. It must never crash.
#   language-features.py what the language reference describes, a piece at a time
#   syntax-errors.py     what is said to be wrong with some ten thousand pieces of source that will not compile, and where
#   operations.py        what comes of every operator between every two of some hundred values, and of the built-in functions of one and of two of them
#   methods.py           what comes of every method of the built-in classes, of several instances of each, given what it is meant for and what it is not quite
#   special-methods.py   what the language makes of what a program's own special methods do: each of them, given each of some eighty things to do, and everything that would call it
#   other-objects.py     what comes of every attribute of everything else that is built in, in each of the states that it can be in: iterators, views, generators, functions, descriptors, frames, classes, exceptions
#   syntax-trees.py      what comes of compiling, and of running, syntax trees that a program has made wrongly, in something over a million ways. It must never crash.
#
# Beside each is what CPython prints, in a .reference file, which is made by running it with CPython.

# The part of the library that is written in Python does not come with the engine. It is CPython's, as it is.
[ -n "$PYTHONPATH" ] || { echo "PYTHONPATH is to name the Lib directory of CPython 3.14" >&2; exit 2; }

jsc=$1
show=${2:-0}
cd "$(dirname "$0")" || exit 2
for audit in own-attributes wrong-arguments language-features syntax-errors syntax-trees operations methods special-methods other-objects; do
    "$jsc" $audit.py > /tmp/python-audit-$audit.txt 2> /tmp/python-audit-$audit.err || echo "$audit: stopped early: $(tail -1 /tmp/python-audit-$audit.err)"
    # Whatever python3 is has a library of its own.
    env -u PYTHONPATH python3 compare.py $audit $audit.reference /tmp/python-audit-$audit.txt "$show"
done
