#!/usr/bin/env python3
"""Run 3.2 wrapper: same measurement path as v3_driver, raw records to
results-v32-raw.jsonl so prior raws stay pristine."""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import v3_driver as d
d.RAW = os.path.join(d.SB, "results-v32-raw.jsonl")
d.main()
