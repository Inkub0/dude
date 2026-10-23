#!/usr/bin/env python3
"""Quake 4 GUI census: window-level properties, events and script commands.

Walks every .gui/.pd/.guide in the Quake 4 paks (later paks override earlier, like the
engine's VFS), separates window-level property statements from script blocks, and reports
which property names DUDE's GUI parser does not accept for that window type, with the
shape of their values (how many comma-separated items, quoted or not). Unknown properties
fall into idWindow::ParseRegEntry, which keeps ONE token as a user variable: fine for a
single value, a parser desync for a comma list. Script blocks are summarised too: event
handler names, onTime forms, and script commands.

Usage:  Q4BASE=/path/to/q4base python3 tools/quake4/q4_gui_census.py [--unknown-only]
"""
import collections
import os
import re
import sys
import zipfile

Q4 = os.environ.get("Q4BASE", "/mnt/linux-data/SteamLibrary/steamapps/common/Quake 4/q4base")

# ---- what DUDE's ui/ parser accepts (keep in sync with neo/ui/*.cpp) -------------------
WINDOW_TYPES = {"windowdef", "animationdef", "editdef", "choicedef", "sliderdef", "markerdef",
                "binddef", "listdef", "fielddef", "renderdef", "gamessddef", "gamebearshootdef",
                "gamebustoutdef"}
SCRIPT_EVENTS = {"onmouseenter", "onmouseexit", "onaction", "onactivate", "ondeactivate", "onesc",
                 "onevent", "ontrigger", "onactionrelease", "onenter", "onenterrelease",
                 # Quake 4 (Q2)
                 "onbackaction", "ontabrelease", "oninit"}
COMMON_VARS = {
    # idWindow::ParseInternalVar
    "showtime", "showcoords", "scaleto43", "forceaspectwidth", "forceaspectheight", "matscalex",
    "matscaley", "bordersize", "nowrap", "shadow", "textalign", "textalignx", "textaligny", "shear",
    "wantenter", "naturalmatscale", "noclip", "nocursor", "menugui", "modal", "invertrect", "name",
    "play", "comment", "font", "cstnoclipbackground",
    # RegisterVars
    "forecolor", "hovercolor", "backcolor", "bordercolor", "rect", "matcolor", "scale", "translate",
    "rotate", "textscale", "visible", "noevents", "text", "background", "runscript", "varbackground",
    "cvar", "choices", "choicevar", "bind", "cstlayer", "cstpseudobit", "cstresetscrollbar",
    "cstwritetop", "cstanchor", "cstanchorto", "cstanchorfactor", "modelrotate",
    # GetWinVarByName built-ins
    "notime", "backgroundname", "hidecursor",
    # parser keywords
    "definefloat", "definevec4", "float",
    # Quake 4 (Q2)
    "textspacing", "textstyle", "alwaysthink", "selected", "fontsize", "defineicon",
}
TYPE_VARS = {
    "listdef": {"horizontal", "listname", "tabstops", "tabaligns", "multiplesel", "tabvaligns",
                "tabtypes", "tabiconsizes", "tabiconvoffset",
                "tabtextscales", "itemheight", "scrollbar", "backgroundhover", "backgroundfocus",
                "backgroundline", "backgroundgreyed"},
    "choicedef": {"choicetype", "currentchoice", "choices", "values", "cvar", "gui", "liveupdate",
                  "updategroup"},
    "sliderdef": {"stepsize", "step", "low", "high", "vertical", "verticalflip", "scrollbar",
                  "thumbshader", "value", "cvar", "liveupdate", "cvargroup"},
    "editdef": {"maxchars", "numeric", "wrap", "readonly", "forcescroll", "source", "password",
                "cvarmax", "cvar", "liveupdate", "cvargroup", "cvarmin", "chatwindow", "showcursor"},
    "binddef": {"bind"},
    "renderdef": {"animclass", "model", "anim", "lightorigin", "lightcolor", "modelorigin",
                  "modelrotate", "viewoffset", "needsrender"},
    "fielddef": {"cursorvar", "showcursor"},
    "markerdef": {"markermat", "markerstop", "markercolor"},
}
SCRIPT_COMMANDS = {"set", "setfocus", "endgame", "resettime", "showcursor", "resetcinematics",
                   "transition", "localsound", "runscript", "evalregs", "if", "else",
                   # Quake 4 (Q2)
                   "namedevent", "stoptransitions", "consolecmd", "resetvideo", "noninteractive",
                   "setlightcolor"}
# names idRenderWindow consumes by pattern (Quake 4 numbered preview lights, four values)
PATTERN_VARS = {"renderdef": re.compile(r"(lightorigin|lightcolor)\d+$")}

TOKEN_RE = re.compile(r'"(?:[^"\\\n]|\\.)*"|-?\d+\.\d*|-?\.\d+|-?\d+|[A-Za-z_$][A-Za-z0-9_:$]*|\S')


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def tokenize(text):
    """[(token, line)]"""
    out = []
    for lineno, line in enumerate(strip_comments(text).split("\n"), 1):
        for m in TOKEN_RE.finditer(line):
            out.append((m.group(0), lineno))
    return out


class Census:
    def __init__(self):
        self.unknown = collections.defaultdict(lambda: collections.Counter())   # (wtype,name) -> shapes
        self.unknown_files = collections.defaultdict(set)
        self.known = collections.Counter()
        self.events = collections.Counter()
        self.ontime = collections.Counter()
        self.commands = collections.Counter()
        self.unknown_commands = collections.Counter()
        self.samples = {}

    def shape(self, toks):
        vals = [t for t, _ in toks]
        if not vals:
            return "(nothing)"
        items = " ".join(vals).split(",")
        quoted = all(v.startswith('"') for v in vals if v != ",")
        return f"{len(items)} item(s){' quoted' if quoted else ''}"

    def skip_block(self, toks, i):
        """toks[i] must be '{'; returns index after the matching '}', counting script commands."""
        assert toks[i][0] == "{"
        depth = 0
        stmt_start = True
        while i < len(toks):
            t = toks[i][0]
            if t == "{":
                depth += 1
                stmt_start = True
            elif t == "}":
                depth -= 1
                stmt_start = True
                if depth == 0:
                    return i + 1
            elif t == ";":
                stmt_start = True
            elif stmt_start:
                low = t.lower()
                if re.match(r"[a-z_]+$", low):
                    self.commands[low] += 1
                    if low not in SCRIPT_COMMANDS:
                        self.unknown_commands[low] += 1
                stmt_start = False
            i += 1
        return i

    def window(self, toks, i, wtype, fname):
        """toks[i] is the window name; returns index after the closing brace."""
        i += 1
        if i >= len(toks) or toks[i][0] != "{":
            return i
        i += 1
        while i < len(toks) and toks[i][0] != "}":
            t, line = toks[i]
            low = t.lower()
            if low in WINDOW_TYPES:
                i = self.window(toks, i + 1, low, fname)
                continue
            if low == "ontime":
                if i + 1 < len(toks) and toks[i + 1][0] == "+":
                    self.ontime["onTime +N"] += 1
                    i += 3
                else:
                    self.ontime["onTime N"] += 1
                    i += 2
                if i < len(toks) and toks[i][0] == "{":
                    i = self.skip_block(toks, i)
                continue
            if low == "onnamedevent":
                self.events["onnamedevent"] += 1
                i += 2
                if i < len(toks) and toks[i][0] == "{":
                    i = self.skip_block(toks, i)
                continue
            if low.startswith("on") and i + 1 < len(toks) and toks[i + 1][0] == "{":
                self.events[low] += 1
                if low not in SCRIPT_EVENTS:
                    self.unknown[(wtype, low)]["script block"] += 1
                    self.unknown_files[(wtype, low)].add(fname)
                i = self.skip_block(toks, i + 1)
                continue
            # property statement: the rest of this line
            j = i + 1
            while j < len(toks) and toks[j][1] == line and toks[j][0] not in ("{", "}"):
                j += 1
            value = toks[i + 1:j]
            known = (low in COMMON_VARS or low in TYPE_VARS.get(wtype, set())
                     or (wtype in PATTERN_VARS and PATTERN_VARS[wtype].match(low)))
            if known:
                self.known[low] += 1
            else:
                key = (wtype, low)
                self.unknown[key][self.shape(value)] += 1
                self.unknown_files[key].add(fname)
                self.samples.setdefault(key, " ".join(v for v, _ in value)[:70])
            i = j
        return i + 1

    def file(self, name, text):
        toks = tokenize(text)
        i = 0
        while i < len(toks):
            low = toks[i][0].lower()
            if low in WINDOW_TYPES:
                i = self.window(toks, i + 1, low, name)
            else:
                i += 1


def main():
    unknown_only = "--unknown-only" in sys.argv
    seen = set()
    c = Census()
    for pak in sorted((p for p in os.listdir(Q4) if p.endswith(".pk4")), reverse=True):
        z = zipfile.ZipFile(os.path.join(Q4, pak))
        for n in z.namelist():
            if n.lower().endswith((".gui", ".pd", ".guide")) and n not in seen:
                seen.add(n)
                c.file(n, z.read(n).decode("latin-1"))
    print(f"files: {len(seen)}")
    # ParseRegEntry keeps exactly one token, so an unknown single value is harmless (it becomes
    # an unused user variable); anything longer, or a script block, desynchronises the parser
    risky = {k: v for k, v in c.unknown.items()
             if any(shape not in ("1 item(s)", "1 item(s) quoted") for shape in v)}
    print(f"\n== unknown window properties that DESYNCHRONISE the parser: {len(risky)}")
    for key, shapes in sorted(risky.items(), key=lambda kv: -sum(kv[1].values())):
        print(f"  {key[0]:12s} {key[1]:24s} {sum(shapes.values()):5d}  {dict(shapes)}"
              f"  files={len(c.unknown_files[key])}  e.g. {c.samples.get(key, '')!r}")
    print("\n== unknown single-value properties (harmless, kept as unused user variables)")
    for key, shapes in sorted(c.unknown.items(), key=lambda kv: -sum(kv[1].values())):
        if key not in risky:
            print(f"  {key[0]:12s} {key[1]:24s} {sum(shapes.values()):5d}  e.g. {c.samples.get(key, '')!r}")
    if unknown_only:
        return
    print("\n== events:", dict(c.events.most_common()))
    print("== onTime forms:", dict(c.ontime))
    print("== script commands DUDE does not implement:", dict(c.unknown_commands.most_common()))
    print("== all script commands:", dict(c.commands.most_common(25)))


if __name__ == "__main__":
    main()
