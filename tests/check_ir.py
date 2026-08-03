#!/usr/bin/env python3
"""Minimal textual assertions for the scheduler's focused IR regressions."""

import re
import sys


def function(text, name):
    match = re.search(rf"^define\b[^@]*@{re.escape(name)}\([^\n]*\).*?^}}$",
                      text, re.MULTILINE | re.DOTALL)
    if not match:
        raise AssertionError(f"function {name} not found")
    return match.group(0)


def in_order(text, *needles):
    offset = 0
    for needle in needles:
        found = text.find(needle, offset)
        if found < 0:
            raise AssertionError(f"expected after offset {offset}: {needle}")
        offset = found + len(needle)


def check_memory(text):
    in_order(function(text, "call_then_load"),
             "call void @external_write", "%value = load i32, ptr %p", "ret i32")
    in_order(function(text, "main"),
             "call void @write_slot", "%value = load i32, ptr @slot", "ret i32")
    in_order(function(text, "volatile_atomic_fence"),
             "store volatile i32 1", "%first = load atomic i32",
             "fence seq_cst", "store atomic i32 2",
             "%second = load volatile i32", "ret i32")


def check_loop(text):
    body = function(text, "loop_phi_diamond")
    latch = body[body.index("latch:"):]
    in_order(latch, "%selected = phi", "%next = add i32 %i, 1", "br label %loop")


def check_hoist(text):
    safe = function(text, "safe_hoist")
    entry = safe[safe.index("entry:"):safe.index("work:")]
    in_order(entry, "%hoisted = add i32 %x, 7", "br label %work")

    unsafe = function(text, "unsafe_division")
    entry = unsafe[unsafe.index("entry:"):unsafe.index("work:")]
    if "%quotient = sdiv" in entry:
        raise AssertionError("unsafe division was speculated into entry")
    work = unsafe[unsafe.index("work:"):]
    in_order(work, "%quotient = sdiv i32 84, %x", "ret i32 %quotient")


CHECKS = {
    "memory": check_memory,
    "loop": check_loop,
    "hoist": check_hoist,
}


def main():
    mode, path = sys.argv[1:]
    CHECKS[mode](open(path, encoding="utf-8").read())


if __name__ == "__main__":
    main()
