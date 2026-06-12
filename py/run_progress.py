# -*- coding: utf-8 -*-
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tool.msg_util import axsbe_file
from behave.axob import AXOB
from tool.axsbe_base import SecurityIDSource_SZSE, INSTRUMENT_TYPE

DATA_FILE = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                         "data", "20220422", "AX_sbe_szse_000001.log")

print(f"Reading: {DATA_FILE}")
axob = AXOB(1, SecurityIDSource_SZSE, INSTRUMENT_TYPE.STOCK)

total = 0
t0 = time.time()
for msg in axsbe_file(DATA_FILE):
    axob.onMsg(msg)
    total += 1
    if total % 10000 == 0:
        print(f"  processed {total} msgs...")
        sys.stdout.flush()

elapsed = time.time() - t0
print(f"\n=== Results ===")
print(f"Total: {total} msgs")
print(f"Time:  {elapsed:.3f} s ({int(total/elapsed)} msg/s)")
print(axob)
sys.stdout.flush()
