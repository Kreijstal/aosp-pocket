# SPDX-License-Identifier: GPL-3.0-only
"""Exact-match source edits for the 12L camera HAL port."""

def edit(text, name, pairs):
    for old, new in pairs:
        if text.count(old) != 1:
            raise SystemExit(f'{name}: expected one match for {old[:70]!r}, found {text.count(old)}')
        text = text.replace(old, new)
    return text

def region(text, start, end_marker):
    """Text from the line starting with start up to and including the closing brace of the function
    that starts with end_marker."""
    a = text.index('\n' + start) + 1
    b = text.index('\n' + end_marker, a) + 1
    c = text.index('\n}\n', b) + 3
    return a, c
