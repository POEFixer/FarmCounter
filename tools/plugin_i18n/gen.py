"""Plugin translations: the embedded English header and the translation gate.

    python tools/plugin_i18n/gen.py <PluginDir>... | --all [--check] [--gate]

A localized plugin keeps one flat JSON file per program language in <PluginDir>/lang/
(en.json, pt_BR.json, ru.json, ... : the codes of POEFixer/core/Languages.h, copied below as
PROGRAM_CODES so this script also runs in a published plugin repository) and compiles its
English in through a generated header (PluginI18n.h: Tr falls back to it).

  (no flag)  writes lang/en.json as src/lang_en.generated.h (<PluginDir>/lang_en.generated.h when
             the plugin has no src/): sorted by key, C++ escapes, LF.
  --check    the header is current (CRLF normalized); writes nothing.
  --gate     the translation gate; writes nothing. Fails on:
               - a program code without lang/<code>.json, or an extra or non-ASCII file in lang/
                 (lang/dynamic_prefixes.txt is allowed, below);
               - a file that is not one flat JSON object of strings (what PluginI18n.h refuses);
               - a key missing or extra in any language, or a {name} set other than English's;
               - a printf conversion (%d, %s, ...), "###", leading or trailing whitespace or two
                 spaces in a row in a value (layout is code: Indent / SameLine);
               - a Tr("...") / TrF("..." / TrId("...") / Key("...") key in the plugin's sources
                 missing from lang/en.json;
               - a key of lang/en.json that no Tr / TrF / TrId / Key literal names, unless it starts
                 with a prefix listed in lang/dynamic_prefixes.txt (one per line, # comments: the
                 prefixes of keys the plugin builds at run time; a prefix that matches no key fails);
               - a TrF("key", { { "name", ... }, ... }) whose literal argument names are not exactly
                 the {placeholders} of the key's English text.
             A plugin without lang/ (and without Tr calls) passes with a warning line.
--all takes every folder of Plugins/ that holds a .vcxproj. One line per failure; exit 1 when
anything failed, 2 on bad usage. POEFIXER_REPO_ROOT overrides the repository root (tests).
"""
import argparse
import json
import os
import re
import sys
from pathlib import Path

ROOT = Path(os.environ.get('POEFIXER_REPO_ROOT') or Path(__file__).resolve().parents[2])
# The program's UI languages in its selector order: a copy of kProgramLanguages in
# POEFixer/core/Languages.h (test_gen.py compares the two), so gen.py needs no host file and works in
# a published plugin repository.
PROGRAM_CODES = ('en', 'pt_BR', 'ru', 'th', 'de', 'fr', 'es', 'ja', 'ko', 'zh_TW')
LANGUAGES_H = 'POEFixer/core/Languages.h'
HEADER_NAME = 'lang_en.generated.h'
DYNAMIC_PREFIXES = 'dynamic_prefixes.txt'
MAX_FILE_BYTES = 1024 * 1024
PLACEHOLDER = re.compile(r'\{([A-Za-z_][A-Za-z0-9_]*)\}')
PRINTF = re.compile(r'%[-+ #0]*\d*(?:\.\d+)?[diouxXeEfgGcsp]')
PREFIX = re.compile(r'^[A-Za-z0-9_.-]+$')
SOURCE_SUFFIXES = {'.cpp', '.cc', '.cxx', '.c', '.h', '.hh', '.hpp', '.hxx', '.inl', '.ipp'}
SKIPPED_DIRS = {'lib', 'x64', 'out', 'build', 'Debug', 'Release', 'third_party', 'external'}
TR_CALL = re.compile(r'\b(TrF|TrId|Tr|Key)\s*\(\s*(?:u8)?"((?:[^"\\\n]|\\.)*)"')


class ObjectPairs(list):
    """A JSON object as its (key, value) pairs, in file order."""


class Failures:
    def __init__(self):
        self.lines = []

    def add(self, plugin, message):
        self.lines.append('%s: %s' % (plugin, message))


def program_codes():
    """The program language codes, in selector order (PROGRAM_CODES)."""
    return list(PROGRAM_CODES)


def languages_h_codes(root=None):
    """The codes of Languages.h kProgramLanguages, in selector order: what PROGRAM_CODES copies
    (tests only; a published plugin repository has no Languages.h)."""
    text = ((root or ROOT) / LANGUAGES_H).read_text(encoding='utf-8')
    start = text.index('kProgramLanguages')
    block = text[text.index('{{', start):text.index('}};', start)]
    codes = re.findall(r'^\s*\{\s*"([A-Za-z_]+)"\s*,', block, re.M)
    if not codes or codes[0] != 'en':
        raise RuntimeError('%s: kProgramLanguages not recognized' % LANGUAGES_H)
    return codes


def load_flat(path):
    """(ordered list of (key, value), None) or (None, the reason PluginI18n.h would refuse it)."""
    raw = path.read_bytes()
    if len(raw) > MAX_FILE_BYTES:
        return None, 'larger than 1 MiB'
    if raw.startswith(b'\xef\xbb\xbf'):
        raw = raw[3:]
    try:
        text = raw.decode('utf-8')
    except UnicodeDecodeError as error:
        return None, 'not UTF-8 (%s)' % error.reason

    def pairs_hook(pairs):
        seen = set()
        for key, _ in pairs:
            if key in seen:
                raise ValueError('duplicate key "%s"' % key)
            seen.add(key)
        return ObjectPairs(pairs)

    try:
        data = json.loads(text, object_pairs_hook=pairs_hook)
    except ValueError as error:
        return None, 'not valid JSON (%s)' % error
    if not isinstance(data, ObjectPairs):
        return None, 'not one JSON object of strings'
    for key, value in data:
        if not isinstance(value, str):
            return None, 'the value of "%s" is not a string' % key
        for part in (key, value):
            try:
                part.encode('utf-8')
            except UnicodeEncodeError:
                return None, 'a lone surrogate in "%s"' % key
            if '\x00' in part:
                return None, 'U+0000 in "%s"' % key
    return data, None


def cpp_literal(text):
    out = ['"']
    for byte in text.encode('utf-8'):
        c = chr(byte)
        if c == '\\':
            out.append('\\\\')
        elif c == '"':
            out.append('\\"')
        elif c == '\n':
            out.append('\\n')
        elif c == '\r':
            out.append('\\r')
        elif c == '\t':
            out.append('\\t')
        elif 0x20 <= byte < 0x7F:
            out.append(c)
        else:
            out.append('\\%03o' % byte)   # three octal digits: never runs into the next character
    out.append('"')
    return ''.join(out)


def render_header(pairs):
    lines = [
        '// Generated by tools/plugin_i18n/gen.py from lang/en.json: do not edit (rerun the tool).',
        '// Include it ONCE, in one .cpp, after PluginSDK.h: it registers the plugin\'s English texts,',
        '// the fallback of every other language (PluginI18n.h).',
        '#pragma once',
        '#ifndef POEFIXER_PLUGIN_I18N_H',
        '#error "Include PluginSDK.h (or PluginI18n.h) before lang_en.generated.h"',
        '#endif',
        '',
    ]
    entries = sorted(pairs, key=lambda kv: kv[0].encode('utf-8'))
    if entries:
        lines.append('inline constexpr PluginSDK::I18n::Entry kPluginLangEn[] = {')
        for key, value in entries:
            lines.append('    { %s, %s },' % (cpp_literal(key), cpp_literal(value)))
        lines.append('};')
        lines.append('inline const PluginSDK::I18n::EmbeddedEnglish g_pluginLangEn{ kPluginLangEn, '
                     'std::size(kPluginLangEn) };')
    else:
        lines.append('inline const PluginSDK::I18n::EmbeddedEnglish g_pluginLangEn{ nullptr, 0 };')
    return ('\n'.join(lines) + '\n').encode('ascii')


def header_path(plugin_dir):
    src = plugin_dir / 'src'
    return (src if src.is_dir() else plugin_dir) / HEADER_NAME


def shown(plugin_dir, path):
    return path.relative_to(plugin_dir).as_posix()


def is_identifier_char(c):
    return c.isalnum() or c == '_'


def literal_prefix_start(text, i):
    """Where the encoding prefix (u8, u, U, L) before text[i] starts, i when there is none."""
    for prefix in ('u8', 'u', 'U', 'L'):
        start = i - len(prefix)
        if start >= 0 and text[start:i] == prefix and (start == 0 or not is_identifier_char(text[start - 1])):
            return start
    return i


def starts_raw_string(text, i):
    if not text.startswith('R"', i):
        return False
    start = literal_prefix_start(text, i)
    return start == 0 or not is_identifier_char(text[start - 1])


def strip_comments(text):
    """The text with // and /* */ comments and raw string literals blanked (newlines kept); string
    and character literals are left alone (a digit separator 1'000 is no character literal)."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith('//', i):
            j = text.find('\n', i)
            j = n if j < 0 else j
            out.append(' ' * (j - i))
            i = j
        elif text.startswith('/*', i):
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append(''.join('\n' if ch == '\n' else ' ' for ch in text[i:j]))
            i = j
        elif c == "'" and i > 0 and is_identifier_char(text[i - 1]) and literal_prefix_start(text, i) == i:
            out.append(c)   # a digit separator
            i += 1
        elif starts_raw_string(text, i):
            open_paren = text.find('(', i + 2)
            if open_paren < 0:
                out.append(c)
                i += 1
                continue
            delimiter = text[i + 2:open_paren]
            end = text.find(')' + delimiter + '"', open_paren)
            end = n if end < 0 else end + len(delimiter) + 2
            out.append(''.join('\n' if ch == '\n' else ' ' for ch in text[i:end]))   # never a key
            i = end
        elif c in '"\'':
            j = i + 1
            while j < n and text[j] != c and text[j] != '\n':
                j += 2 if text[j] == '\\' else 1
            j = min(j + 1, n)
            out.append(text[i:j])
            i = j
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def skip_literal(text, i):
    """The index after the string or character literal whose quote is text[i]."""
    quote, j, n = text[i], i + 1, len(text)
    while j < n and text[j] != quote and text[j] != '\n':
        j += 2 if text[j] == '\\' else 1
    return min(j + 1, n)


CLOSERS = {'(': ')', '[': ']', '{': '}'}


def braced_elements(text, i):
    """(the stripped top-level comma-separated elements of the braced list that opens at text[i],
    the index after its closing brace), or (None, i) when the list does not close where it should.
    Brackets nest; string and character literals are skipped (a digit separator 1'000 is none)."""
    stack, elements, start, j, n = [], [], i + 1, i, len(text)
    while j < n:
        c = text[j]
        if c == "'" and j > 0 and is_identifier_char(text[j - 1]) and literal_prefix_start(text, j) == j:
            j += 1
            continue
        if c in '"\'':
            j = skip_literal(text, j)
            continue
        if c in CLOSERS:
            stack.append(CLOSERS[c])
        elif c in ')]}':
            if not stack or stack[-1] != c:
                return None, i
            stack.pop()
            if not stack:
                elements.append(text[start:j])
                return [e.strip() for e in elements if e.strip()], j + 1
        elif c == ',' and len(stack) == 1:
            elements.append(text[start:j])
            start = j + 1
        j += 1
    return None, i


ARG_NAME = re.compile(r'^(?:u8)?"((?:[^"\\\n]|\\.)*)"$')


def trf_argument_names(text, end):
    """The argument names of the TrF call whose key literal ends at text[end]: a list when they are
    literals of one braced list (TrF("k", { { "a", x }, { "b", y } }) -> ["a", "b"]), None when the gate
    cannot read them (a variable, a non-literal name)."""
    n, j = len(text), end
    while j < n and text[j].isspace():
        j += 1
    if j < n and text[j] == ')':
        return []
    if j >= n or text[j] != ',':
        return None
    j += 1
    while j < n and text[j].isspace():
        j += 1
    if j >= n or text[j] != '{':
        return None
    elements, _ = braced_elements(text, j)
    if elements is None:
        return None
    names = []
    for element in elements:
        if not element.startswith('{'):
            return None
        inner, after = braced_elements(element, 0)
        if not inner or element[after:].strip():
            return None
        match = ARG_NAME.match(inner[0])
        if not match:
            return None
        names.append(match.group(1))
    return names


def source_keys(plugin_dir):
    """[(relative path, line, function, key, TrF argument names or None)] of every Tr / TrF / TrId /
    Key call with a literal key."""
    found = []
    for path in sorted(plugin_dir.rglob('*')):
        if not path.is_file() or path.suffix.lower() not in SOURCE_SUFFIXES or path.name == HEADER_NAME:
            continue
        parts = path.relative_to(plugin_dir).parts[:-1]
        if any(part in SKIPPED_DIRS or part.startswith('.') for part in parts):
            continue
        text = strip_comments(path.read_text(encoding='utf-8', errors='replace'))
        for match in TR_CALL.finditer(text):
            line = text.count('\n', 0, match.start()) + 1
            names = trf_argument_names(text, match.end()) if match.group(1) == 'TrF' else None
            found.append((path.relative_to(plugin_dir).as_posix(), line, match.group(1), match.group(2), names))
    return found


def dynamic_prefixes(lang, name, english_map, failures):
    """The key prefixes listed in lang/dynamic_prefixes.txt (keys the plugin builds at run time); each
    must be a key prefix that matches a key of lang/en.json."""
    path = lang / DYNAMIC_PREFIXES
    if not path.is_file():
        return []
    try:
        lines = path.read_bytes().decode('utf-8-sig').splitlines()
    except UnicodeDecodeError:
        failures.add(name, 'lang/%s: not UTF-8' % DYNAMIC_PREFIXES)
        return []
    prefixes = []
    for number, raw in enumerate(lines, 1):
        prefix = raw.split('#', 1)[0].strip()
        if not prefix:
            continue
        if not PREFIX.match(prefix):
            failures.add(name, 'lang/%s:%d: "%s" is no key prefix (letters, digits, _ . -)'
                         % (DYNAMIC_PREFIXES, number, prefix))
        elif not any(key.startswith(prefix) for key in english_map):
            failures.add(name, 'lang/%s:%d: "%s" matches no key of lang/en.json' % (DYNAMIC_PREFIXES, number, prefix))
        else:
            prefixes.append(prefix)
    return prefixes


def value_problems(value):
    problems = []
    conversion = PRINTF.search(value)
    if conversion:
        problems.append('a printf conversion "%s" (texts are never formats: use {name} and TrF)' % conversion.group(0))
    if '###' in value:
        problems.append('"###" (TrId adds the id itself)')
    if value != value.strip():
        problems.append('leading or trailing whitespace (layout is code: ImGui::Indent / SameLine)')
    if '  ' in value:
        problems.append('two spaces in a row (layout is code: SameLine, or two keys)')
    return problems


def gate(plugin_dir, codes, failures):
    name = plugin_dir.name
    lang = plugin_dir / 'lang'
    calls = source_keys(plugin_dir)
    if not lang.is_dir():
        for rel, line, function, key, _ in calls:
            failures.add(name, '%s:%d: %s("%s") but the plugin has no lang/en.json' % (rel, line, function, key))
        if not calls:
            print('warning: %s: no lang/ folder: the plugin shows English only' % name)
        return
    allowed = {code + '.json' for code in codes}
    for entry in sorted(lang.iterdir(), key=lambda p: p.name):
        if not entry.name.isascii():
            failures.add(name, 'lang/%s: a non-ASCII file name' % entry.name)
        elif entry.name == DYNAMIC_PREFIXES and entry.is_file():
            continue
        elif entry.is_dir() or entry.name not in allowed:
            failures.add(name, 'lang/%s: not a program language file (%s) nor %s'
                         % (entry.name, ', '.join(sorted(allowed)), DYNAMIC_PREFIXES))
    tables = {}
    for code in codes:
        path = lang / (code + '.json')
        if not path.is_file():
            failures.add(name, 'lang/%s.json: missing' % code)
            continue
        pairs, error = load_flat(path)
        if error:
            failures.add(name, 'lang/%s.json: %s' % (code, error))
            continue
        tables[code] = pairs
    english = tables.get('en')
    if english is None:
        return
    english_map = dict(english)
    for code in codes:
        pairs = tables.get(code)
        if pairs is None:
            continue
        table = dict(pairs)
        if code != 'en':
            for key, _ in english:
                if key not in table:
                    failures.add(name, 'lang/%s.json: missing key "%s"' % (code, key))
            for key, _ in pairs:
                if key not in english_map:
                    failures.add(name, 'lang/%s.json: extra key "%s" (not in en.json)' % (code, key))
        for key, value in pairs:
            if code != 'en' and key in english_map:
                mine = sorted(set(PLACEHOLDER.findall(value)))
                theirs = sorted(set(PLACEHOLDER.findall(english_map[key])))
                if mine != theirs:
                    failures.add(name, 'lang/%s.json: "%s": placeholders {%s} differ from English {%s}'
                                 % (code, key, ', '.join(mine), ', '.join(theirs)))
            for problem in value_problems(value):
                failures.add(name, 'lang/%s.json: "%s": %s' % (code, key, problem))
    reported = set()
    for rel, line, function, key, _ in calls:
        if key not in english_map and (rel, line, key) not in reported:
            reported.add((rel, line, key))
            failures.add(name, '%s:%d: %s("%s"): the key is not in lang/en.json' % (rel, line, function, key))
    # Every English key is used: named by a Tr / TrF / TrId / Key literal, or built at run time under
    # a listed prefix.
    used = {key for _, _, _, key, _ in calls}
    prefixes = dynamic_prefixes(lang, name, english_map, failures)
    for key, _ in english:
        if key not in used and not any(key.startswith(prefix) for prefix in prefixes):
            failures.add(name, 'lang/en.json: "%s" is never used: no Tr / TrF / TrId / Key("%s") in the sources '
                               '(a table of keys takes I18n::Key; a key built at run time needs its prefix in '
                               'lang/%s)' % (key, key, DYNAMIC_PREFIXES))
    # A TrF call names exactly its text's placeholders: a missing argument shows "{name}" on screen, an
    # extra one is a misspelt name.
    checked = set()
    for rel, line, function, key, names in calls:
        if function != 'TrF' or names is None or key not in english_map or (rel, line, key) in checked:
            continue
        checked.add((rel, line, key))
        wanted = set(PLACEHOLDER.findall(english_map[key]))
        missing = sorted(wanted - set(names))
        extra = sorted(set(names) - wanted)
        if missing or extra:
            parts = []
            if missing:
                parts.append('no argument for %s' % ', '.join('{%s}' % n for n in missing))
            if extra:
                parts.append('"%s" not in the text' % '", "'.join(extra))
            has = '{%s}' % ', '.join(sorted(wanted)) if wanted else 'no placeholder'
            failures.add(name, '%s:%d: TrF("%s"): %s (the text has %s)' % (rel, line, key, '; '.join(parts), has))


def generate(plugin_dir, check, failures):
    name = plugin_dir.name
    en_path = plugin_dir / 'lang' / 'en.json'
    target = header_path(plugin_dir)
    if not en_path.is_file():
        if target.exists():
            failures.add(name, '%s exists but lang/en.json does not: delete the header or add the file'
                         % shown(plugin_dir, target))
        elif not check:
            print('%s: no lang/en.json (skipped)' % name)
        return
    pairs, error = load_flat(en_path)
    if error:
        failures.add(name, 'lang/en.json: %s' % error)
        return
    data = render_header(pairs)
    current = target.read_bytes().replace(b'\r\n', b'\n') if target.is_file() else None
    if check:
        if current is None:
            failures.add(name, '%s is missing: run python tools/plugin_i18n/gen.py %s'
                         % (shown(plugin_dir, target), plugin_dir.as_posix()))
        elif current != data:
            failures.add(name, '%s is stale: run python tools/plugin_i18n/gen.py %s'
                         % (shown(plugin_dir, target), plugin_dir.as_posix()))
        return
    if current == data:
        print('%s: %s is current' % (name, shown(plugin_dir, target)))
        return
    target.write_bytes(data)
    print('%s: wrote %s (%d texts)' % (name, shown(plugin_dir, target), len(pairs)))


def every_plugin(root=None):
    plugins = (root or ROOT) / 'Plugins'
    return [d for d in sorted(plugins.iterdir(), key=lambda p: p.name) if d.is_dir() and any(d.glob('*.vcxproj'))]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('plugins', nargs='*', help='plugin folders (Plugins/<Name>)')
    parser.add_argument('--all', action='store_true', help='every plugin folder under Plugins/')
    parser.add_argument('--check', action='store_true', help='the generated header is current; write nothing')
    parser.add_argument('--gate', action='store_true', help='the translation gate; write nothing')
    args = parser.parse_args(argv)
    if bool(args.plugins) == args.all:
        parser.print_usage(sys.stderr)
        print('gen.py: name plugin folders or --all, not both', file=sys.stderr)
        return 2
    folders = every_plugin() if args.all else [Path(p) for p in args.plugins]
    for folder in folders:
        if not folder.is_dir():
            print('gen.py: %s is not a folder' % folder, file=sys.stderr)
            return 2
    codes = program_codes()
    failures = Failures()
    for folder in folders:
        if args.gate:
            gate(folder, codes, failures)
        if args.check or not args.gate:
            generate(folder, args.check, failures)
    for line in failures.lines:
        print(line)
    return 1 if failures.lines else 0


if __name__ == '__main__':
    sys.exit(main())
