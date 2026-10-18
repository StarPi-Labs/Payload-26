"""
The block catalog: what each block is called, what it asks for, what it does.

The browser builds its Scratch-like blocks from GET /api/catalog, so this file
is the single source of truth - adding a block here makes it appear in the
editor, with its sentence, its fields and its help text.

A block's `template` is the sentence shown on the block; {name} places the
parameter `name` as an editable field, "|" starts a new line.
"""
import math
from dataclasses import dataclass, field

from .signals import BlockError

CATEGORIES = [
    # id, label, colour (Scratch's palette, so it feels familiar)
    ("data", "Data", "#FFAB19"),
    ("select", "Select", "#FF8C1A"),
    ("fix", "Fix & calibrate", "#4C97FF"),
    ("clean", "Clean", "#9966FF"),
    ("derive", "Derive", "#59C059"),
    ("estimate", "Estimate", "#FF6680"),
    ("analyze", "Analyze", "#0FBD8C"),
]
CATEGORY_IDS = [c[0] for c in CATEGORIES]


@dataclass
class Param:
    name: str
    kind: str                 # number | int | bool | choice | signal | signals | text | recording
    default: object = None
    help: str = ""
    min: float = None
    max: float = None
    precision: float = None   # number fields: step of the value
    options: list = field(default_factory=list)   # choice: [(value, label), ...]
    unit: str = ""
    filter: str = ""          # signal(s): which names to offer, e.g. "imu.*"
    optional: bool = False    # signal: may be left at "(none)"
    # Only relevant when another parameter has one of these values, e.g.
    # {"method": ["mahony"]}: the editor hides the line it sits on otherwise.
    show_if: dict = None

    def to_json(self):
        d = {"name": self.name, "kind": self.kind, "default": self.default, "help": self.help,
             "unit": self.unit}
        for k in ("min", "max", "precision"):
            if getattr(self, k) is not None:
                d[k] = getattr(self, k)
        if self.options:
            d["options"] = [[str(v), lbl] for v, lbl in self.options]
        if self.filter:
            d["filter"] = self.filter
        if self.optional:
            d["optional"] = True
        if self.show_if:
            d["showIf"] = {k: [str(x) for x in v] for k, v in self.show_if.items()}
        return d

    def coerce(self, raw):
        """The parameter's value from what the editor sent (strings, mostly)."""
        if raw is None or raw == "":
            raw = self.default
        try:
            if self.kind == "number":
                v = float(raw)
                if not math.isfinite(v):
                    raise ValueError
            elif self.kind == "int":
                v = int(float(raw))
            elif self.kind == "bool":
                v = raw if isinstance(raw, bool) else str(raw).upper() in ("TRUE", "1", "YES", "ON")
                return v
            elif self.kind == "choice":
                v = str(raw)
                allowed = [str(o[0]) for o in self.options]
                if v not in allowed:
                    v = str(self.default)
                return v
            else:
                v = "" if raw is None else str(raw)
                if self.kind == "signal" and v == "(none)":
                    v = ""
                return v
        except (TypeError, ValueError):
            raise BlockError(f"'{self.name}' must be a number, got '{raw}'") from None
        if self.min is not None and v < self.min:
            raise BlockError(f"'{self.name}' must be at least {self.min}")
        if self.max is not None and v > self.max:
            raise BlockError(f"'{self.name}' must be at most {self.max}")
        return v


@dataclass
class Result:
    changed: list = field(default_factory=list)    # signals created or modified
    notes: list = field(default_factory=list)      # one-line findings, plain language
    warnings: list = field(default_factory=list)
    table: list = None                             # [[header...], [row...], ...]
    download: dict = None                          # {"name": ..., "content": ...}

    def to_json(self):
        return {"changed": self.changed, "notes": self.notes, "warnings": self.warnings,
                "table": self.table, "download": self.download}


@dataclass
class BlockType:
    type: str
    category: str
    title: str
    template: str
    help: str
    params: list
    fn: object = None
    hat: bool = False

    def to_json(self):
        return {"type": self.type, "category": self.category, "title": self.title,
                "template": self.template, "help": self.help.strip(), "hat": self.hat,
                "params": [p.to_json() for p in self.params]}

    def parse(self, raw_params):
        raw_params = raw_params or {}
        return {p.name: p.coerce(raw_params.get(p.name)) for p in self.params}


REGISTRY = {}


def block(type, category, title, template, help, params=(), hat=False):
    """Register a block function: fn(ws, params, ctx) -> Result."""
    assert category in CATEGORY_IDS, category

    def deco(fn):
        bt = BlockType(type, category, title, template, help, list(params), fn, hat)
        names = {p.name for p in bt.params}
        for part in template.replace("|", " ").split("{")[1:]:
            assert part.split("}")[0] in names, f"{type}: template field {part} has no parameter"
        REGISTRY[type] = bt
        return fn
    return deco


def catalog_json():
    return {"categories": [{"id": c, "label": label, "colour": colour}
                           for c, label, colour in CATEGORIES],
            "blocks": [bt.to_json() for bt in REGISTRY.values()]}


# Parameters many blocks share ------------------------------------------------

def p_signals(default="imu.a*", filter="", help="Which signals: one name, several separated by commas, or a pattern such as imu.a* (all three accelerometer axes)."):
    return Param("signals", "signals", default, help, filter=filter)


def p_signal(name="signal", default="", filter="", help="", optional=False):
    return Param(name, "signal", default, help, filter=filter, optional=optional)


def p_keep(help="Keep the original signals too: the result gets a new name ending in .{suffix} instead of replacing them."):
    return Param("keep", "bool", False, help)
