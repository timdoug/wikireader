#!/usr/bin/env python3
"""Modernize pre-ANSI C sources for current binutils/GCC.

Handles the three mechanical transforms the EPSON C33 sources need:

  1. ``PARAMS ((a, b))``  ->  ``(a, b)``      (PARAMS is gone from ansidecl.h)
  2. K&R function definitions -> ANSI prototypes
  3. BFD's old ``boolean`` type -> C99 ``bool``

Run:  modernize.py FILE...      (edits in place)
"""
import re
import sys
import pathlib

# ---------------------------------------------------------------- PARAMS


def strip_params(s):
    """PARAMS ((x, y)) -> (x, y), with balanced-paren matching."""
    out, i = [], 0
    while True:
        m = re.compile(r'\bPARAMS\s*\(\s*\(').search(s, i)
        if not m:
            out.append(s[i:])
            break
        out.append(s[i:m.start()])
        # Walk from the inner '(' to its match.
        depth, j = 0, m.end() - 1
        while j < len(s):
            if s[j] == '(':
                depth += 1
            elif s[j] == ')':
                depth -= 1
                if depth == 0:
                    break
            j += 1
        inner = s[m.end() - 1:j + 1]          # "(x, y)"
        # Skip the closing paren of PARAMS( ... ) itself.
        k = j + 1
        while k < len(s) and s[k] in ' \t\n':
            k += 1
        if k < len(s) and s[k] == ')':
            k += 1
        out.append(inner)
        i = k
    return ''.join(out)


# ------------------------------------------------------------ K&R -> ANSI

def split_declarators(decl):
    """'int a, *b' -> ['int a', 'int *b'] (approximately, but good enough).

    Returns a list of (name, full_text) pairs.
    """
    decl = decl.strip()
    if not decl:
        return []
    # Split top-level commas.
    parts, depth, cur = [], 0, ''
    for ch in decl:
        if ch in '([':
            depth += 1
        elif ch in ')]':
            depth -= 1
        if ch == ',' and depth == 0:
            parts.append(cur)
            cur = ''
        else:
            cur += ch
    parts.append(cur)

    # Base type = leading tokens of the first declarator, minus its declarator
    # part (stars, name, array/function suffix).
    first = parts[0].strip()
    m = re.match(r'^((?:\b(?:const|volatile|register|struct|union|enum|unsigned|'
                 r'signed|long|short)\b\s+)*[A-Za-z_]\w*)\s+(.*)$', first)
    if not m:
        return []
    base = m.group(1)

    result = []
    for idx, p in enumerate(parts):
        p = p.strip()
        if idx == 0:
            declarator = m.group(2).strip()
        else:
            declarator = p
        nm = re.search(r'([A-Za-z_]\w*)\s*(?:\[|\()?', declarator.lstrip('*& '))
        if not nm:
            return []
        result.append((nm.group(1), f'{base} {declarator}'))
    return result


KR_HEADER = re.compile(
    r'^(?P<name>[A-Za-z_]\w*)[ \t]*\((?P<args>\s*[A-Za-z_]\w*'
    r'(?:\s*,\s*[A-Za-z_]\w*)*\s*)\)[ \t]*$', re.M)


def kr_to_ansi(s):
    lines = s.split('\n')
    out, i, n = [], 0, len(lines)
    converted = 0
    while i < n:
        line = lines[i]
        m = KR_HEADER.match(line)
        if not m:
            out.append(line)
            i += 1
            continue

        names = [a.strip() for a in m.group('args').split(',')]
        # Collect the declaration block up to the opening brace.
        j, decls = i + 1, []
        while j < n:
            t = lines[j].strip()
            if t == '{':
                break
            if t == '' or t.startswith('/*') or t.startswith('*'):
                decls.append(lines[j])
                j += 1
                continue
            if not t.endswith(';'):
                break
            decls.append(lines[j])
            j += 1
        else:
            out.append(line)
            i += 1
            continue

        if j >= n or lines[j].strip() != '{' or not decls:
            out.append(line)
            i += 1
            continue

        # Parse the declarations into name -> text.
        blob = '\n'.join(decls)
        blob = re.sub(r'/\*.*?\*/', ' ', blob, flags=re.S)
        table = {}
        ok = True
        for stmt in blob.split(';'):
            stmt = stmt.strip()
            if not stmt:
                continue
            pairs = split_declarators(stmt)
            if not pairs:
                ok = False
                break
            for nm, text in pairs:
                table[nm] = re.sub(r'\s+', ' ', text).strip()

        if not ok or any(nm not in table for nm in names):
            out.append(line)
            i += 1
            continue

        params = ', '.join(table[nm] for nm in names)
        header = f'{m.group("name")} ({params})'
        if len(header) > 78:
            indent = ' ' * (len(m.group('name')) + 2)
            joined = (',\n' + indent).join(table[nm] for nm in names)
            header = f'{m.group("name")} ({joined})'
        out.append(header)
        converted += 1
        i = j            # continue at the '{'
    return '\n'.join(out), converted


# ---------------------------------------------------------------- driver

def main():
    for path in sys.argv[1:]:
        p = pathlib.Path(path)
        s = orig = p.read_text(encoding='latin-1')
        s = strip_params(s)
        s, nconv = kr_to_ansi(s)
        s = re.sub(r'\bboolean\b', 'bool', s)
        if s != orig:
            p.write_text(s, encoding='latin-1')
        print(f'{p.name:20} K&R->ANSI: {nconv:3}   '
              f'{"changed" if s != orig else "unchanged"}')


if __name__ == '__main__':
    main()
