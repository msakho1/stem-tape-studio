#!/usr/bin/env python3
"""
Catch "called before declared" in main.c WITHOUT a cross-compiler.

WHY THIS EXISTS. main.c can only be compiled by the Zephyr ARM toolchain, so
every ordinary C mistake in it costs a full CI round-trip to find. That is
fine for anything genuinely target-specific and pure waste for what is not --
and this bug class is not: a call to a file-local static that sits ABOVE its
definition, with no prototype in between. C99 makes it an implicit
declaration (a warning) and then a hard "conflicting types" error at the
definition, so it always fails the build, always for the same reason, and
always somewhere the local host tests cannot see.

It has already happened once, exactly this way: stem_song_post_commit_reload()
called stem_prime_group0(), which is defined ~1000 lines further down with the
rest of the flash path. The fix is a forward declaration, not a move.

WHAT IT DOES NOT DO. This is a text scanner, not a parser. It knows about
file-scope `static <type> name(...)` definitions and prototypes, and it
ignores anything inside comments and string literals. It does not resolve
macros, and it will not find type errors, missing headers, or anything else a
compiler finds. It removes one specific, repeated, expensive-to-find mistake
from the CI-only set; the build remains the authority for everything else.
"""
import re
import sys

# `static <stuff> name(` at file scope -- the opening brace (definition) or
# semicolon (prototype) is decided by what follows the closing paren.
DEF_RE = re.compile(
    r"^static\s+(?:[A-Za-z_][A-Za-z0-9_]*\s+|\*|inline\s+|const\s+)+"
    r"(?P<name>[A-Za-z_][A-Za-z0-9_]*)\s*\(",
    re.M)
CALL_RE = re.compile(r"(?<![A-Za-z0-9_])(?P<name>[A-Za-z_][A-Za-z0-9_]*)\s*\(")


def strip_comments_and_strings(src):
    """Blank out comments and string/char literals, preserving offsets so every
    reported position still points at the real line."""
    out = list(src)
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c == "/" and i + 1 < n and src[i + 1] == "/":
            while i < n and src[i] != "\n":
                out[i] = " "
                i += 1
        elif c == "/" and i + 1 < n and src[i + 1] == "*":
            while i < n and not (src[i] == "*" and i + 1 < n and src[i + 1] == "/"):
                if src[i] != "\n":
                    out[i] = " "
                i += 1
            for _ in range(2):
                if i < n:
                    out[i] = " "
                    i += 1
        elif c in "\"'":
            quote = c
            i += 1
            while i < n and src[i] != quote:
                if src[i] == "\\":
                    out[i] = " "
                    i += 1
                if i < n:
                    if src[i] != "\n":
                        out[i] = " "
                    i += 1
            if i < n:
                out[i] = " "
                i += 1
        else:
            i += 1
    return "".join(out)


def main():
    path = sys.argv[1]
    raw = open(path, encoding="utf-8", errors="replace").read()
    src = strip_comments_and_strings(raw)

    # First appearance of each file-local static, and whether it was a
    # prototype (`);`) or a definition.
    first_decl = {}
    definitions = {}
    for m in DEF_RE.finditer(src):
        name = m.group("name")
        # find the matching close paren, then look at the next real character
        depth, k = 0, m.end() - 1
        while k < len(src):
            if src[k] == "(":
                depth += 1
            elif src[k] == ")":
                depth -= 1
                if depth == 0:
                    break
            k += 1
        tail = src[k + 1:k + 40].lstrip()
        is_proto = tail.startswith(";")
        if name not in first_decl:
            first_decl[name] = m.start()
        if not is_proto and name not in definitions:
            definitions[name] = m.start()

    problems = []
    for m in CALL_RE.finditer(src):
        name = m.group("name")
        if name not in definitions:
            continue
        pos = m.start()
        if pos >= first_decl[name]:
            continue
        # A call textually before ANY declaration of a file-local static.
        line = raw.count("\n", 0, pos) + 1
        decl_line = raw.count("\n", 0, definitions[name]) + 1
        problems.append((line, name, decl_line))

    if problems:
        print("FAIL: called before declared -- add a forward declaration")
        for line, name, decl_line in sorted(problems):
            print("  %s:%d calls %s(), which is not declared until line %d"
                  % (path, line, name, decl_line))
        print()
        print("  C99 makes this an implicit declaration and then a hard")
        print("  'conflicting types' error at the definition, so it always")
        print("  fails the ARM build -- this catches it without one.")
        return 1

    print("PASS: every file-local static is declared before it is called")
    print("  %d static definitions checked" % len(definitions))
    return 0


if __name__ == "__main__":
    sys.exit(main())
