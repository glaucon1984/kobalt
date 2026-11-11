"""Put a StarDict dictionary of your own into one of Kobalt's dictionary slots on a PocketBook.

  python convert-dictionary.py <mount> <slot> <dictionary>     convert, back up the slot, install
  python convert-dictionary.py <mount> <slot> --restore        put the backed-up dictionary back
  python convert-dictionary.py --check <folder> <slot> [word]  read a converted folder back

  <mount>       the device over USB, e.g. L: or /media/you/PB710
  <slot>        the language pair the dictionary replaces: en-en, es-es, en-es... It must already
                be installed from Kobalt's own catalogue (Settings > Dictionaries)
  <dictionary>  its .ifo file, or the folder that holds it

Kobalt looks words up in one dictionary per language pair, from its catalogue, and its reader
(src/dictionaries/stardict.cpp) loads one form of StarDict only: version 2.4.2, definitions in
HTML, a dictzip body with its random-access table, an index sorted by ASCII-folded bytes, and its
own offset tables (.idx.kobalt, .syn.kobalt) beside the index, which it builds at install and
never again. So a dictionary cannot simply be copied over another. This tool writes a slot folder
in exactly that form from a StarDict dictionary whose definitions are HTML, XDXF or plain text
(sametypesequence h, x or m), reads the result back by the reader's own rules, and installs it.

- Inflected forms ("harbours" for "harbour"): the dictionary's own .syn is kept, and the forms
  of the catalogue dictionary it replaces are added for the base words it has. --no-forms leaves
  the catalogue's out.
- Several entries under one headword become one, separated by a rule.
- The slot's own dictionary is copied to --backup (default ./kobalt-dictionary-backup) before the
  first install, and --restore puts it back. Removing or downloading the dictionary again in
  Kobalt also brings the catalogue's back.
- Kobalt keeps showing the catalogue's name for the slot; the source line under a definition
  names your dictionary.

Written against the reader of Kobalt v1.0-beta.7. Python 3.8 or later, no packages. Use it with
dictionaries you have the right to use; it contains none.
"""
import argparse
import gzip
import hashlib
import html
import json
import os
import re
import shutil
import struct
import sys
import tempfile
import zlib

CHUNKS = (58315, 65535)  # dictzip's usual chunk, then the largest one a 16-bit size allows
INDENT = 24  # pixels per level of a nested line (XDXF blockquotes)
LABEL = "#555555"  # coloured labels of an XDXF dictionary, on a greyscale screen
MARK = "convert-dictionary.py"
ENTRY_LIMIT = 4 * 1024 * 1024  # the reader refuses a longer definition
SLOTS = os.path.join("applications", "kobalt", "profile", "config", "dictionaries")


class Problem(Exception):
    pass


def key(word):
    # The reader's compare(): ASCII case folded first, then the bytes as they are.
    return (word.lower(), word)


def usable(word):
    if not word or len(word) > 255:
        return False
    try:
        word.decode("utf-8")
    except UnicodeDecodeError:
        return False
    return True


def read_index(data, suffix):
    at = 0
    while at < len(data):
        end = data.find(b"\0", at)
        if end < 0 or end + 1 + suffix > len(data):
            raise Problem("the index is cut short")
        yield data[at:end], data[end + 1:end + 1 + suffix]
        at = end + 1 + suffix


def read_file(prefix, name, required=True):
    for path, opener in ((prefix + name, open), (prefix + name + ".gz", gzip.open), (prefix + name + ".dz", gzip.open)):
        if os.path.exists(path):
            with opener(path, "rb") as file:
                return file.read()
    if required:
        raise Problem("%s%s is missing" % (prefix, name))
    return None


def read_ifo(prefix):
    try:
        text = open(prefix + ".ifo", "rb").read().decode("utf-8", "replace")
    except OSError:
        raise Problem("%s.ifo is missing" % prefix)
    if not text.startswith("StarDict's dict ifo file"):
        raise Problem("%s.ifo is not a StarDict description" % prefix)
    return dict(line.split("=", 1) for line in text.splitlines()[1:] if "=" in line)


# ---- definitions as HTML ----

XDXF = [
    (re.compile(r"<k>.*?</k>", re.S), ""),
    (re.compile(r"<rref\b[^>]*>.*?</rref>", re.S), ""),
    (re.compile(r"</?(?:dtrn|kref|iref|nu|opt|def|deftext|sr|ar)\b[^>]*>"), ""),
    (re.compile(r"<(?:c|tr|co)\b[^>]*>"), '<span style="color:%s">' % LABEL),
    (re.compile(r"</(?:c|tr|co)>"), "</span>"),
    (re.compile(r"<(/?)(?:ex|abr|gr|pos)\b[^>]*>"), r"<\1i>"),
    (re.compile(r"</?blockquote>"), ""),
    (re.compile(r"&apos;"), "'"),
]
TAG = re.compile(r"<(/?)([a-zA-Z]+)\b[^>]*?(/?)>")
HTML_TAG = re.compile(r"</?(?:p|br|b|i|u|strong|em|div|span|font|ul|ol|li|sup|sub|hr|blockquote|table|tr|td)\b[^>]*>", re.I)


def balanced(line):
    """Close what a line leaves open and drop a close without its open, so one line is one block."""
    stack, out, at = [], [], 0
    for match in TAG.finditer(line):
        out.append(line[at:match.start()])
        at = match.end()
        closing, name, empty = match.group(1), match.group(2).lower(), match.group(3)
        if empty or name == "br":
            out.append(match.group(0))
        elif not closing:
            stack.append(name)
            out.append(match.group(0))
        elif name in stack:
            while stack:
                top = stack.pop()
                out.append("</%s>" % top)
                if top == name:
                    break
    out.append(line[at:])
    out.extend("</%s>" % name for name in reversed(stack))
    return "".join(out)


def from_xdxf(raw):
    """One block per source line; the depth of its blockquotes is its indent."""
    blocks = []
    for line in raw.decode("utf-8", "replace").split("\n"):
        line = line.strip()
        depth = 0
        while line.startswith("<blockquote>"):
            line = line[len("<blockquote>"):].lstrip()
            depth += 1
        for pattern, replacement in XDXF:
            line = pattern.sub(replacement, line)
        line = balanced(line.strip())
        if not re.sub(r"<[^>]*>", "", line).strip():
            continue
        style = ' style="margin-left:%dpx"' % (depth * INDENT) if depth else ""
        blocks.append("<div%s>%s</div>" % (style, line))
    return "".join(blocks).encode("utf-8")


def from_html(raw):
    return re.sub(r"\s*\n\s*", " ", raw.decode("utf-8", "replace")).strip().encode("utf-8")


def from_text(raw):
    lines = [html.escape(line.rstrip(), quote=False) for line in raw.decode("utf-8", "replace").strip().split("\n")]
    return "<br/>".join(lines).encode("utf-8")


def pick_format(kind, wanted, index, body):
    if wanted != "auto":
        return wanted
    if kind == "h":
        return "html"
    if kind == "x":
        return "xdxf"
    # "m" is plain text by the format, and HTML in many dictionaries that say so.
    sample = [struct.unpack(">II", value) for _, value in index[::max(1, len(index) // 2000)]]
    marked = sum(1 for offset, size in sample if HTML_TAG.search(body[offset:offset + size].decode("utf-8", "replace")))
    return "html" if marked * 3 > len(sample) else "text"


# ---- writing a slot folder ----

def write_dictzip(path, data):
    for chunk in CHUNKS:
        count = (len(data) + chunk - 1) // chunk
        if 10 + 2 * count <= 0xFFFF:
            break
    else:
        raise Problem("the definitions are too large for one dictzip file (%.1f GB)" % (len(data) / 1e9))
    deflate = zlib.compressobj(9, zlib.DEFLATED, -15)
    pieces = []
    for i in range(count):
        piece = deflate.compress(data[i * chunk:(i + 1) * chunk]) + deflate.flush(zlib.Z_FULL_FLUSH)
        if not 0 < len(piece) <= 0xFFFF:
            raise Problem("a compressed chunk does not fit dictzip's table")
        pieces.append(piece)
    tail = deflate.flush(zlib.Z_FINISH)
    extra = b"RA" + struct.pack("<HHHH", 6 + 2 * count, 1, chunk, count) + b"".join(struct.pack("<H", len(p)) for p in pieces)
    with open(path, "wb") as out:
        out.write(b"\x1f\x8b\x08\x04" + struct.pack("<I", 0) + b"\x00\x03" + struct.pack("<H", len(extra)) + extra)
        for piece in pieces:
            out.write(piece)
        out.write(tail)
        out.write(struct.pack("<II", zlib.crc32(data) & 0xFFFFFFFF, len(data) & 0xFFFFFFFF))


def write_index(path, rows):
    """rows: (word, packed value) in the reader's order. Also writes the .kobalt offsets."""
    index, offsets = bytearray(), bytearray()
    for word, value in rows:
        offsets += struct.pack(">I", len(index))
        index += word + b"\0" + value
    if len(index) > 0xFFFFFFFF:
        raise Problem("the index is too large")
    with open(path, "wb") as file:
        file.write(index)
    with open(path + ".kobalt", "wb") as file:
        file.write(offsets)
    return len(index)


def convert(source, folder, slot, name, wanted="auto", catalogue=None):
    """Writes `folder` (the slot's files) from the dictionary at `source` (a path without extension)."""
    ifo = read_ifo(source)
    if ifo.get("version") not in ("2.4.2", "3.0.0"):
        raise Problem("StarDict version %s is not supported" % ifo.get("version"))
    if ifo.get("idxoffsetbits", "32") != "32":
        raise Problem("a 64-bit index is not supported")
    kind = ifo.get("sametypesequence", "")
    if kind not in ("h", "x", "m"):
        raise Problem("definitions of type '%s' are not supported (HTML 'h', XDXF 'x' and plain text 'm' are)"
                      % (kind or "mixed"))
    index = list(read_index(read_file(source, ".idx"), 8))
    body = read_file(source, ".dict")
    form = pick_format(kind, wanted, index, body)
    transform = {"html": from_html, "xdxf": from_xdxf, "text": from_text}[form]
    name = name or ifo.get("bookname") or os.path.basename(source)

    converted = {}  # (offset, size) in the source -> HTML
    by_word = {}  # headword -> its definitions, in source order, without repeats
    skipped = 0
    for word, value in index:
        place = struct.unpack(">II", value)
        if place not in converted:
            if place[0] + place[1] > len(body):
                raise Problem("the index points outside the definitions")
            converted[place] = transform(body[place[0]:place[0] + place[1]])
        text = converted[place]
        if not usable(word) or not text:
            skipped += 1
            continue
        known = by_word.setdefault(word, [])
        if text not in known:
            known.append(text)
    del body
    words = sorted(by_word, key=key)
    out, placed, rows, merged = bytearray(), {}, [], 0
    for word in list(words):
        text = b"<hr/>".join(by_word[word])
        if len(text) > ENTRY_LIMIT:
            words.remove(word)
            skipped += 1
            continue
        merged += len(by_word[word]) > 1
        if text not in placed:
            placed[text] = len(out)
            out += text
        rows.append((word, struct.pack(">II", placed[text], len(text))))
    if not rows:
        raise Problem("no usable entry in %s" % source)
    os.makedirs(folder, exist_ok=True)
    prefix = os.path.join(folder, slot)
    index_size = write_index(prefix + ".idx", rows)
    write_dictzip(prefix + ".dict.dz", bytes(out))
    print("%s: %s definitions as %s: %d headwords (%d with several entries merged, %d rows left out), %.1f MB of HTML"
          % (name, "'%s'" % kind, form, len(words), merged, skipped, len(out) / 1e6))

    # Inflected forms: the dictionary's own first, then the catalogue's for the base words it has.
    number = {word: i for i, word in enumerate(words)}
    folded = {}
    for i, word in enumerate(words):
        folded.setdefault(word.lower(), i)
    forms, seen, counts = [], set(), []
    for label, prefix_of in (("its own", source), ("the catalogue dictionary's", catalogue)):
        table = read_file(prefix_of, ".syn", required=False) if prefix_of else None
        if not table:
            continue
        heads = [word for word, _ in (index if prefix_of == source else read_index(read_file(prefix_of, ".idx"), 8))]
        before = len(forms)
        for form_word, value in read_index(table, 4):
            at = struct.unpack(">I", value)[0]
            if at >= len(heads):
                continue
            target = number.get(heads[at], folded.get(heads[at].lower()))
            # A form that is a headword here is found before the inflection table is asked.
            if target is None or not usable(form_word) or form_word.lower() in folded or form_word in seen:
                continue
            seen.add(form_word)
            forms.append((form_word, struct.pack(">I", target)))
        counts.append("%d %s" % (len(forms) - before, label))
    forms.sort(key=lambda row: key(row[0]))
    for stale in (".syn", ".syn.kobalt"):
        if os.path.exists(prefix + stale):
            os.remove(prefix + stale)
    if forms:
        write_index(prefix + ".syn", forms)
    print("%s: %d inflected forms%s" % (name, len(forms), " (" + ", ".join(counts) + ")" if counts else ""))

    lines = ["StarDict's dict ifo file", "version=2.4.2", "bookname=" + slot, "wordcount=%d" % len(words)]
    if forms:
        lines.append("synwordcount=%d" % len(forms))
    lines += ["idxfilesize=%d" % index_size, "author=" + MARK, "description=" + name.replace("\n", " "),
              "sametypesequence=h"]
    with open(prefix + ".ifo", "wb") as file:
        file.write(("\n".join(lines) + "\n").encode("utf-8"))
    with open(os.path.join(folder, "ATTRIBUTION"), "wb") as file:
        text = "A dictionary of the user's own, converted for Kobalt by %s.\n\n" % MARK
        text += "It is built from the following sources:\n* own: %s\n" % name.replace("\n", " ")
        if catalogue and any("catalogue" in count and not count.startswith("0 ") for count in counts):
            text += "* forms: inflected forms of the catalogue dictionary %s\n" % slot
        file.write(text.encode("utf-8"))


# ---- reading a slot folder as the reader does ----

def check(folder, slot, probes=()):
    prefix = os.path.join(folder, slot)

    def need(condition, what):
        if not condition:
            raise Problem("%s: %s" % (prefix, what))

    with open(prefix + ".ifo", "rb") as file:
        need(file.read().startswith(b"StarDict's dict ifo file\n"), "invalid StarDict metadata")
    values = read_ifo(prefix)
    need(values.get("version") == "2.4.2" and values.get("sametypesequence") == "h", "unsupported StarDict format")
    words = int(values.get("wordcount", "0"))
    need(words and int(values.get("idxfilesize", "-1")) == os.path.getsize(prefix + ".idx"), "index size differs")

    dz = open(prefix + ".dict.dz", "rb")
    header = dz.read(12)
    need(len(header) == 12 and header[0] == 31 and header[1] == 139 and header[2] == 8
         and header[3] & 4 and not header[3] & ~(4 | 8 | 16), "invalid dictzip header")
    extra = dz.read(struct.unpack("<H", header[10:12])[0])
    need(extra[:2] == b"RA" and len(extra) >= 10, "no random-access table")
    size, version, chunk, count = struct.unpack("<HHHH", extra[2:10])
    need(version == 1 and chunk and count and size == 6 + count * 2 and len(extra) == 4 + size, "invalid chunk table")
    chunks, offset = [], dz.tell()
    for piece in struct.unpack("<%dH" % count, extra[10:]):
        need(piece, "an empty chunk")
        chunks.append(offset)
        offset += piece
    total = os.path.getsize(prefix + ".dict.dz")
    need(offset < total - 8, "no end of stream")
    chunks.append(offset)
    dz.seek(total - 4)
    length = struct.unpack("<I", dz.read(4))[0]
    need((count - 1) * chunk < length <= count * chunk, "length differs from the chunk table")

    def read(at, size):
        need(size and size <= ENTRY_LIMIT and at + size <= length, "a definition outside the data")
        result = b""
        for i in range(at // chunk, (at + size - 1) // chunk + 1):
            dz.seek(chunks[i])
            inflater = zlib.decompressobj(-15)
            data = inflater.decompress(dz.read(chunks[i + 1] - chunks[i]))
            need(not inflater.unused_data and len(data) == min(chunk, length - i * chunk), "a damaged chunk")
            start = max(at, i * chunk) - i * chunk
            result += data[start:min(at + size, i * chunk + len(data)) - i * chunk]
        return result

    def table(path, suffix, expected):
        with open(path, "rb") as file:
            data = file.read()
        with open(path + ".kobalt", "rb") as file:
            offsets = file.read()
        need(len(offsets) == expected * 4, "%s: count differs" % os.path.basename(path))
        previous, at, n = None, 0, 0
        for word, value in read_index(data, suffix):
            need(n < expected and struct.unpack(">I", offsets[n * 4:n * 4 + 4])[0] == at, "offset table differs")
            need(usable(word) and (previous is None or key(previous) <= key(word)), "index out of order")
            if suffix == 8:
                a, b = struct.unpack(">II", value)
                need(a + b <= length, "index points outside its data")
            else:
                need(struct.unpack(">I", value)[0] < words, "inflection points outside the index")
            previous, at, n = word, at + len(word) + 1 + suffix, n + 1
        need(n == expected, "word count differs from its index")
        return data, offsets

    idx, idx_at = table(prefix + ".idx", 8, words)
    forms = int(values.get("synwordcount", "0"))
    need(forms or not os.path.exists(prefix + ".syn"), "an inflection table the description does not count")
    syn = table(prefix + ".syn", 4, forms) if forms else None

    def find(data, offsets, word):
        for exact in (True, False):
            low, high = 0, len(offsets) // 4
            while low < high:
                middle = (low + high) // 2
                at = struct.unpack(">I", offsets[middle * 4:middle * 4 + 4])[0]
                candidate = data[at:data.index(b"\0", at)]
                if (key(candidate) < key(word)) if exact else (candidate.lower() < word.lower()):
                    low = middle + 1
                else:
                    high = middle
            if low < len(offsets) // 4:
                at = struct.unpack(">I", offsets[low * 4:low * 4 + 4])[0]
                candidate = data[at:data.index(b"\0", at)]
                if (candidate == word) if exact else (candidate.lower() == word.lower()):
                    return low
        return -1

    print("%s: reads as Kobalt reads it: %d headwords, %d inflected forms, %.1f MB"
          % (slot, words, forms, sum(os.path.getsize(os.path.join(folder, f)) for f in os.listdir(folder)) / 1e6))
    for probe in probes:
        word = probe.encode("utf-8")
        number, via = find(idx, idx_at, word), ""
        if number < 0 and syn:
            hit = find(syn[0], syn[1], word)
            if hit >= 0:
                at = struct.unpack(">I", syn[1][hit * 4:hit * 4 + 4])[0]
                end = syn[0].index(b"\0", at)
                number, via = struct.unpack(">I", syn[0][end + 1:end + 5])[0], " (inflected form)"
        if number < 0:
            print("  %s: not found" % probe)
            continue
        at = struct.unpack(">I", idx_at[number * 4:number * 4 + 4])[0]
        end = idx.index(b"\0", at)
        a, b = struct.unpack(">II", idx[end + 1:end + 9])
        plain = re.sub(r"\s+", " ", html.unescape(re.sub(r"<[^>]*>", " ", read(a, b).decode("utf-8")))).strip()
        print("  %s -> %s%s: %s" % (probe, idx[at:end].decode("utf-8"), via, plain[:72]))
    dz.close()


# ---- the device ----

def slot_folder(mount, slot):
    if mount.endswith(":"):
        mount += os.sep  # "L:" alone would be drive-relative on Windows
    folder = os.path.join(mount, SLOTS, slot)
    try:
        installed = json.load(open(os.path.join(folder, "installed.json"), encoding="utf-8")).get("id")
    except (OSError, ValueError):
        installed = None
    if installed != slot:
        raise Problem("%s is not installed on %s: install it in Kobalt first (Settings > Dictionaries), "
                      "since a slot is a dictionary of Kobalt's catalogue" % (slot, mount))
    return folder


def is_converted(folder, slot):
    try:
        return read_ifo(os.path.join(folder, slot)).get("author") == MARK
    except Problem:
        return False


def digest(path):
    hash = hashlib.sha256()
    with open(path, "rb") as file:
        for block in iter(lambda: file.read(1 << 20), b""):
            hash.update(block)
    return hash.digest()


def replace_files(source, target, keep=()):
    """Makes `target` hold the files of `source`, and of its own only those in `keep`."""
    names = [name for name in os.listdir(source) if os.path.isfile(os.path.join(source, name))]
    for name in os.listdir(target):
        if name not in names and name not in keep and os.path.isfile(os.path.join(target, name)):
            os.remove(os.path.join(target, name))
    for name in names:
        shutil.copyfile(os.path.join(source, name), os.path.join(target, name))
        if digest(os.path.join(source, name)) != digest(os.path.join(target, name)):
            raise Problem("%s did not arrive intact on the device" % name)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0], epilog="Run with --explain for the full description.")
    parser.add_argument("mount", nargs="?")
    parser.add_argument("slot", nargs="?")
    parser.add_argument("dictionary", nargs="?", help="the dictionary's .ifo file or its folder")
    parser.add_argument("words", nargs="*", help="words to look up in the result")
    parser.add_argument("--name", help="how the source line names the dictionary (default: its bookname)")
    parser.add_argument("--format", choices=("auto", "html", "xdxf", "text"), default="auto",
                        help="what the definitions are (default: from the dictionary's description)")
    parser.add_argument("--no-forms", action="store_true", help="do not add the catalogue dictionary's inflected forms")
    parser.add_argument("--backup", default="kobalt-dictionary-backup", help="where the slot's own dictionary is kept")
    parser.add_argument("--output", help="write the converted folder here and leave the device alone")
    parser.add_argument("--restore", action="store_true", help="put the backed-up dictionary back in the slot")
    parser.add_argument("--check", action="store_true", help="read a converted folder back: --check <folder> <slot> [word...]")
    parser.add_argument("--explain", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.explain:
        print(__doc__)
        return
    if not args.mount or not args.slot:
        parser.error("a mount (or, with --check, a folder) and a slot are needed")
    if not re.fullmatch(r"[a-z]{2,3}-[a-z]{2,3}", args.slot):
        parser.error("a slot is a language pair such as en-en or es-en")

    if args.check:
        check(args.mount, args.slot, ([args.dictionary] if args.dictionary else []) + args.words)
        return
    device = slot_folder(args.mount, args.slot)
    kept = os.path.join(args.backup, args.slot)
    if args.restore:
        if not os.path.exists(os.path.join(kept, args.slot + ".ifo")):
            raise Problem("no backup of %s in %s" % (args.slot, args.backup))
        replace_files(kept, device)
        print("%s: the backed-up dictionary is back in its slot" % args.slot)
        return
    if not args.dictionary:
        parser.error("which dictionary? Give its .ifo file or its folder")

    source = args.dictionary
    if os.path.isdir(source):
        found = [name for name in os.listdir(source) if name.endswith(".ifo")]
        if len(found) != 1:
            raise Problem("%s holds %d .ifo files; name the one to convert" % (source, len(found)))
        source = os.path.join(source, found[0])
    source = source[:-4] if source.endswith(".ifo") else source

    converted = is_converted(device, args.slot)
    have_backup = os.path.exists(os.path.join(kept, args.slot + ".ifo"))
    catalogue = None
    if not args.no_forms:
        catalogue = os.path.join(kept if converted else device, args.slot)
        if converted and not have_backup:
            catalogue = None
            print("%s holds a converted dictionary and %s has no backup of the catalogue's: its inflected "
                  "forms are not added" % (args.slot, args.backup))

    folder = args.output or tempfile.mkdtemp(prefix="kobalt-dictionary-")
    try:
        convert(source, folder, args.slot, args.name, args.format, catalogue)
        check(folder, args.slot, args.words)
        if args.output:
            print("written to %s; the device is as it was" % folder)
            return
        if not converted and not have_backup:
            os.makedirs(kept, exist_ok=True)
            replace_files(device, kept)
            print("%s: the catalogue dictionary is kept in %s" % (args.slot, kept))
        replace_files(folder, device, keep=("installed.json",))
        check(device, args.slot)
        print("%s: installed. Eject the device; --restore puts the catalogue dictionary back" % args.slot)
    finally:
        if not args.output:
            shutil.rmtree(folder, ignore_errors=True)


if __name__ == "__main__":
    try:
        main()
    except Problem as problem:
        sys.exit("convert-dictionary: %s" % problem)
