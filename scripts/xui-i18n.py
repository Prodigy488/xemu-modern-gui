#!/usr/bin/env python3
"""Interface translations for the xemu dashboard (ui/xui).

  scripts/xui-i18n.py extract   list the strings marked in the source into
                                ui/xui/lang/strings.json
  scripts/xui-i18n.py check     report missing and broken translations
  scripts/xui-i18n.py gen       write ui/xui/i18n-tables.inc (also checks)

Strings are marked with tr("..."), trid("..."), tr_items("a\\0b\\0") or
N_("..."). A leading icon and an ImGui "##id" suffix are not part of the
text to translate, the same as tr() at runtime.

Each language is ui/xui/lang/<code>.json: {"English": "Translation"}.
Translations must keep the same printf conversions (%d, %s, %.1f...) in
the same order, or they're left out (English is shown instead).
"""
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
XUI = os.path.join(ROOT, 'ui', 'xui')
LANG = os.path.join(XUI, 'lang')
LANGS = ['es', 'pt-BR', 'fr', 'de', 'it', 'pl', 'ru', 'ja', 'zh-CN', 'ko']
MARKERS = ('tr', 'trid', 'tr_items', 'N_')


def load_macros():
    """ICON_* string macros, so tr(ICON_FA_PLAY " Play") can be read."""
    macros = {}
    paths = [os.path.join(ROOT, 'ui', 'thirdparty', 'fa', 'IconsFontAwesome6.h'),
             os.path.join(XUI, 'font-manager.hh'),
             os.path.join(ROOT, 'ui', 'xemu-net.h')]
    rx = re.compile(r'#define\s+((?:ICON|XEMU)_\w+)\s+((?:"(?:[^"\\]|\\.)*"\s*)+)')
    for p in paths:
        with open(p, encoding='utf-8', errors='replace') as f:
            for m in rx.finditer(f.read()):
                macros[m.group(1)] = unescape_all(m.group(2))
    return macros


def unescape(body):
    """C string literal body -> bytes"""
    out = bytearray()
    i = 0
    while i < len(body):
        c = body[i]
        if c != '\\':
            out += c.encode('utf-8')
            i += 1
            continue
        i += 1
        e = body[i]
        simple = {'n': 10, 't': 9, 'r': 13, '0': 0, '\\': 92, '"': 34,
                  "'": 39, 'a': 7, 'b': 8, 'f': 12, 'v': 11, '?': 63}
        if e == 'x':
            j = i + 1
            while j < len(body) and body[j] in '0123456789abcdefABCDEF':
                j += 1
            out.append(int(body[i + 1:j], 16) & 0xFF)
            i = j
        elif e in '01234567':
            j = i
            while j < len(body) and j < i + 3 and body[j] in '01234567':
                j += 1
            out.append(int(body[i:j], 8) & 0xFF)
            i = j
        else:
            out.append(simple.get(e, ord(e)))
            i += 1
    return bytes(out)


def unescape_all(s):
    return b''.join(unescape(m.group(1))
                    for m in re.finditer(r'"((?:[^"\\]|\\.)*)"', s))


def tokens(src):
    """C++ tokens we care about: identifiers, strings, punctuation."""
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if src.startswith('//', i):
            i = src.find('\n', i)
            i = n if i < 0 else i
        elif src.startswith('/*', i):
            j = src.find('*/', i + 2)
            i = n if j < 0 else j + 2
        elif c == '"' or (c in 'uUL' and src[i + 1:i + 2] == '"'):
            if c != '"':
                i += 1
            j = i + 1
            while src[j] != '"':
                j += 2 if src[j] == '\\' else 1
            yield ('str', src[i + 1:j], i)
            i = j + 1
        elif c == 'R' and src[i + 1:i + 2] == '"':
            d = src[i + 2:src.index('(', i)]
            end = src.index(')' + d + '"', i)
            yield ('raw', '', i)
            i = end + len(d) + 2
        elif c == "'":
            j = i + 1
            while src[j] != "'":
                j += 2 if src[j] == '\\' else 1
            i = j + 1
        elif c.isalpha() or c == '_':
            j = i
            while j < n and (src[j].isalnum() or src[j] == '_'):
                j += 1
            yield ('id', src[i:j], i)
            i = j
        elif c.isspace():
            i += 1
        else:
            yield ('p', c, i)
            i += 1


def split_core(b):
    """bytes -> the part tr() looks up (no icon prefix, no ##suffix)"""
    i = 0
    while True:
        if len(b) >= i + 3 and ((b[i] == 0xEF and 0x80 <= b[i + 1] <= 0xA4) or
                                b[i] == 0xEE):
            i += 3
            while i < len(b) and b[i] == 0x20:
                i += 1
        else:
            break
    h = b.find(b'##', i)
    core = b[i:] if h < 0 else b[i:h]
    return core.strip(b' \n')


def extract_file(path, macros):
    with open(path, encoding='utf-8') as f:
        src = f.read()
    toks = list(tokens(src))
    found = []
    for k, (kind, val, pos) in enumerate(toks):
        if kind != 'id' or val not in MARKERS:
            continue
        if k + 1 >= len(toks) or toks[k + 1][1] != '(':
            continue
        if k > 0 and toks[k - 1][1] in ('.', '>'):  # member, not ours
            continue
        if k > 0 and toks[k - 1][0] == 'id' and toks[k - 1][1] in (
                'char', 'void', 'define'):
            continue
        parts, j, ok = [], k + 2, True
        while j < len(toks) and toks[j][1] not in (')', ','):
            t = toks[j]
            if t[0] == 'str':
                parts.append(unescape(t[1]))
            elif t[0] == 'id' and t[1] in macros:
                parts.append(macros[t[1]])
            else:
                ok = False
                break
            j += 1
        if not ok or not parts:
            continue
        text = b''.join(parts)
        line = src.count('\n', 0, pos) + 1
        items = text.split(b'\0') if val == 'tr_items' or b'\0' in text \
            else [text]
        for it in items:
            core = split_core(it)
            # Only text: "1280x720" and "4x" aren't worth translating
            letters = re.sub(rb'%[-+ #0-9.*]*[a-zA-Z]+', b'', core)
            if core and re.search(rb'[A-Za-z]{2}', letters):
                found.append((core.decode('utf-8'), '%s:%d' % (
                    os.path.relpath(path, ROOT), line)))
    return found


def source_files():
    out = []
    for name in sorted(os.listdir(XUI)):
        if name.endswith(('.cc', '.hh')):
            out.append(os.path.join(XUI, name))
    return out


def extract():
    macros = load_macros()
    strings = {}
    for p in source_files():
        for core, where in extract_file(p, macros):
            strings.setdefault(core, where)
    return strings


FMT = re.compile(r'%[-+ #0]*(?:\*|\d+)?(?:\.(?:\*|\d+))?'
                 r'(?:hh|h|ll|l|z|j|t|L|I64|I32)?[diouxXeEfFgGaAcspn%]')


def conversions(s):
    return [m.group(0) for m in FMT.finditer(s) if m.group(0) != '%%']


def check_lang(code, strings, quiet=False):
    path = os.path.join(LANG, code + '.json')
    if not os.path.exists(path):
        return {}, ['%s: no file' % code]
    with open(path, encoding='utf-8') as f:
        data = json.load(f)
    good, problems = {}, []
    for en, t in data.items():
        if en not in strings:
            problems.append('%s: no longer used: %r' % (code, en))
            continue
        if not isinstance(t, str) or not t.strip():
            continue
        if conversions(en) != conversions(t):
            problems.append('%s: %s %r: conversions %s vs %s' % (
                code, strings[en], en, conversions(en), conversions(t)))
            continue
        if '##' in t:
            problems.append('%s: %r has ##' % (code, en))
            continue
        if en.count('\n') != t.count('\n') and en.endswith('\n') != \
                t.endswith('\n'):
            problems.append('%s: %r: line breaks differ' % (code, en))
        good[en] = t
    missing = [en for en in strings if en not in good]
    if missing and not quiet:
        problems.append('%s: %d of %d not translated, e.g. %r' % (
            code, len(missing), len(strings), missing[:3]))
    return good, problems


def c_escape(s):
    out = []
    for b in s.encode('utf-8'):
        if b in (34, 92):
            out.append('\\' + chr(b))
        elif 32 <= b < 127 and b != 63:  # no ? (trigraphs)
            out.append(chr(b))
        else:
            out.append('\\%03o' % b)
    return '"' + ''.join(out) + '"'


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else 'check'
    strings = extract()
    if cmd == 'extract':
        with open(os.path.join(LANG, 'strings.json'), 'w',
                  encoding='utf-8') as f:
            json.dump({k: v for k, v in sorted(strings.items())}, f,
                      ensure_ascii=False, indent=1)
            f.write('\n')
        print('%d strings' % len(strings))
        return 0
    bad = 0
    tables = []
    for code in LANGS:
        good, problems = check_lang(code, strings)
        for p in problems:
            print(p)
        bad += sum(1 for p in problems if 'conversions' in p or '##' in p)
        tables.append((code, good))
        print('%s: %d/%d' % (code, len(good), len(strings)))
    if cmd == 'gen':
        out = ['// Generated by scripts/xui-i18n.py gen from ui/xui/lang/*.json.',
               '// Do not edit.', '',
               'struct XuiLangTable { const char *code; '
               'const XuiTrPair *pairs; int count; };', '',
               'static const int kTrTotalStrings = %d;' % len(strings), '']
        for code, good in tables:
            ident = 'kTr_' + code.replace('-', '_')
            out.append('static const XuiTrPair %s[] = {' % ident)
            for en in sorted(good):
                out.append('    { %s,\n      %s },' % (c_escape(en),
                                                     c_escape(good[en])))
            out.append('    { nullptr, nullptr }')
            out.append('};')
            out.append('')
        out.append('static const XuiLangTable kLangTables[] = {')
        for code, good in tables:
            out.append('    { "%s", kTr_%s, %d },' % (
                code, code.replace('-', '_'), len(good)))
        out.append('};')
        with open(os.path.join(XUI, 'i18n-tables.inc'), 'w') as f:
            f.write('\n'.join(out) + '\n')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
