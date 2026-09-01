#!/usr/bin/env python3
"""Run 3.1 (code-delta) wrapper: same measurement path as v3_driver, raw
records go to results-v31b-raw.jsonl so run-3 raw stays pristine."""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import v3_driver as d
d.RAW = os.path.join(d.SB, "results-v31b-raw.jsonl")
d.main()
