"""Tiny S-expression helpers shared by the AudioCarrier generators."""


def parse(s):
    i = 0
    n = len(s)

    def rd():
        nonlocal i
        while s[i] in ' \t\n\r':
            i += 1
        if s[i] == '(':
            i += 1
            out = []
            while True:
                while s[i] in ' \t\n\r':
                    i += 1
                if s[i] == ')':
                    i += 1
                    return out
                out.append(rd())
        if s[i] == '"':
            j = i + 1
            while s[j] != '"':
                j += 2 if s[j] == '\\' else 1
            r = s[i + 1:j]
            i = j + 1
            return r
        j = i
        while j < n and s[j] not in ' \t\n\r()':
            j += 1
        r = s[i:j]
        i = j
        return r

    return rd()


def grab_symbol(libfile, name):
    """Return the raw text of one top-level symbol of a .kicad_sym file."""
    s = open(libfile).read()
    key = '\n  (symbol "%s" ' % name
    i = s.find(key)
    if i < 0:
        key = '\n  (symbol "%s"\n' % name
        i = s.find(key)
    if i < 0:
        raise KeyError(name)
    i += 1
    d = 0
    for j in range(i, len(s)):
        if s[j] == '(':
            d += 1
        elif s[j] == ')':
            d -= 1
            if d == 0:
                return s[i:j + 1]
    raise ValueError(name)
