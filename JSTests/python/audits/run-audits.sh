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
#
# Beside each is what CPython prints, in a .reference file, which is made by running it with CPython.

jsc=$1
show=${2:-0}
cd "$(dirname "$0")" || exit 2
for audit in own-attributes wrong-arguments language-features; do
    "$jsc" $audit.py > /tmp/python-audit-$audit.txt 2> /tmp/python-audit-$audit.err || echo "$audit: stopped early: $(tail -1 /tmp/python-audit-$audit.err)"
    python3 compare.py $audit $audit.reference /tmp/python-audit-$audit.txt "$show"
done
