#!/usr/bin/env python3
"""Read ``spec/usage-model.md`` (if available) and sample certification cases.

The specification file is still being authored, so the loader is deliberately
defensive: it recognises Markdown distribution/transition tables, simple
``key: value`` lists, and JSON/YAML code fences.  Anything it cannot use is
reported as a warning.  If no usable resolution/preset/frame distributions
are found, a documented built-in usage profile is used instead and every
output is flagged ``fallback_used: true`` -- never silently.

This module never writes to ``spec/``.
"""

from __future__ import annotations

import collections
import copy
import hashlib
import json
import re
from pathlib import Path

ATTR_ALIASES = {
    "resolution": ("resolution", "res", "size", "dimensions", "wxh", "video size"),
    "frames": ("frames", "frame count", "framecount", "nframes", "length"),
    "fps": ("fps", "framerate", "frame rate"),
    "preset": ("preset", "presets", "speed preset"),
    "crf": ("crf", "qp", "quality", "rate factor"),
    "content": ("content", "content type", "source", "material", "scenario"),
    "threads": ("threads", "thread", "thread count"),
    "keyint": ("keyint", "gop", "gop size", "max keyint"),
    "min_keyint": ("min-keyint", "min keyint", "minimum keyint"),
    "scenecut": ("scenecut", "scene cut"),
    "extra_args": ("extra args", "extra-args", "extra cli args", "flags"),
}
_WEIGHT_ALIASES = ("prob", "probability", "weight", "p", "share", "likelihood", "frequency")
_STATE_ALIASES = ("state", "profile", "class", "mode", "name", "id")
_SEP_RE = re.compile(r"^\s*\|?\s*:?-{3,}:?\s*(?:\|\s*:?-{3,}:?\s*)+\|?\s*$")
_NUM_RE = re.compile(r"^([0-9]*\.?[0-9]+)\s*(%)?$")


# ---------------------------------------------------------------------------
# Built-in fallback usage profile (documented in cert/README.md)
# ---------------------------------------------------------------------------

def builtin_profile() -> dict:
    """A three-state Markov profile used only when the spec is unusable."""
    common = {
        "fps": [(30, 1.0)], "threads": [(6, 1.0)],
        "keyint": [(250, 1.0)], "min_keyint": [(25, 1.0)],
        "scenecut": [(0, 1.0)], "extra_args": [([], 1.0)],
    }
    states = [
        {
            "name": "short", "weight": 0.25,
            "dist": dict(common,
                         resolution=[((320, 180), 0.4), ((640, 360), 0.6)],
                         frames=[(30, 0.5), (60, 0.5)],
                         preset=[("ultrafast", 0.2), ("veryfast", 0.3),
                                 ("faster", 0.3), ("medium", 0.2)],
                         crf=[(23.0, 0.6), (25.0, 0.4)],
                         content=[("synthetic", 0.6), ("flat", 0.4)]),
        },
        {
            "name": "standard", "weight": 0.55,
            "dist": dict(common,
                         resolution=[((640, 360), 0.5), ((854, 480), 0.3),
                                     ((1280, 720), 0.2)],
                         frames=[(120, 0.4), (240, 0.6)],
                         preset=[("ultrafast", 0.08), ("superfast", 0.09),
                                 ("veryfast", 0.12), ("faster", 0.12),
                                 ("fast", 0.12), ("medium", 0.17),
                                 ("slow", 0.13), ("slower", 0.10),
                                 ("veryslow", 0.07)],
                         crf=[(21.0, 0.3), (23.0, 0.5), (25.0, 0.2)],
                         content=[("synthetic", 0.45), ("motion", 0.30),
                                  ("grain", 0.15), ("edges", 0.10)]),
        },
        {
            "name": "heavy", "weight": 0.20,
            "dist": dict(common,
                         resolution=[((1280, 720), 0.75), ((1920, 1080), 0.25)],
                         frames=[(120, 0.5), (240, 0.5)],
                         preset=[("medium", 0.2), ("slow", 0.3),
                                 ("slower", 0.3), ("veryslow", 0.2)],
                         crf=[(20.0, 0.3), (23.0, 0.7)],
                         content=[("synthetic", 0.5), ("motion", 0.35),
                                  ("grain", 0.15)]),
        },
    ]
    for st in states:
        st["spec_attrs"] = set()
    return {
        "states": states,
        "initial": {"short": 0.25, "standard": 0.55, "heavy": 0.20},
        "transitions": {
            "short": {"short": 0.55, "standard": 0.35, "heavy": 0.10},
            "standard": {"short": 0.25, "standard": 0.55, "heavy": 0.20},
            "heavy": {"short": 0.10, "standard": 0.35, "heavy": 0.55},
        },
        "global": {},
        "global_spec_attrs": set(),
    }


# Factored model (spec/usage-model.md §1-2): six dimensions, each with its own
# stationary weights, stickiness and explicit transition matrix.
FACTORED_ORDER = ("resolution", "frames", "preset", "crf", "threads", "content")
FACTORED_SYMBOLS = {"R": "resolution", "N": "frames", "P": "preset",
                    "C": "crf", "T": "threads", "K": "content"}


def _parse_factored(text: str) -> tuple[dict | None, list[str], list[str]]:
    """Parse the factored Markov model used by spec/usage-model.md."""
    warnings, parsed = [], []
    symbol_to_attr = dict(FACTORED_SYMBOLS)
    code_values: dict[str, dict[str, object]] = {}
    for header, rows in _tables(text):
        low = [h.strip().lower() for h in header]
        if "symbol" in low and "dimension" in low:
            sym_i, dim_i = low.index("symbol"), low.index("dimension")
            val_i = next((i for i, h in enumerate(low)
                          if "value" in h or "meaning" in h), None)
            for row in rows:
                if max(sym_i, dim_i) >= len(row):
                    continue
                sym = row[sym_i].strip("` ").upper()[:1]
                attr = _classify(row[dim_i])
                if sym in FACTORED_SYMBOLS and attr in FACTORED_ORDER:
                    symbol_to_attr[sym] = attr
                if val_i is not None and val_i < len(row) and sym:
                    codes = {}
                    for code, value in re.findall(
                            r"([A-Za-z]\d+)\s*=\s*([^,;]+)", row[val_i]):
                        coerced = _coerce(value.strip().strip("`"), attr)
                        if coerced is not None:
                            codes[code.upper()] = coerced
                    if codes:
                        code_values[sym] = codes
            parsed.append("state symbols and value codes")

    dims = {}
    for header, rows in _tables(text):
        low = [h.strip().lower() for h in header]
        weight_names = ("w", "weight", "prob", "probability", "p")
        if (len(header) >= 4 and len(header) % 2 == 0
                and all(low[i] in weight_names for i in range(1, len(header), 2))
                and all(low[i].strip("` ").upper()[:1] in symbol_to_attr
                        for i in range(0, len(header), 2))):
            parsed.append("stationary weights w_d")
            for row in rows:
                for i in range(0, len(header) - 1, 2):
                    sym = low[i].strip("` ").upper()[:1]
                    attr = symbol_to_attr[sym]
                    if i + 1 >= len(row):
                        continue
                    raw = row[i].strip().strip("`")
                    if not raw or raw in ("—", "-", "–"):
                        continue
                    value = _coerce(raw, attr)
                    if value is None:
                        warnings.append(f"ignored unusable {attr} value {raw!r}")
                        continue
                    try:
                        weight = float(row[i + 1].strip().rstrip("%"))
                    except (TypeError, ValueError):
                        continue
                    d = dims.setdefault(attr, {"values": [], "weights": [],
                                               "transitions": {}, "stickiness": None,
                                               "symbol": sym})
                    if value in d["values"]:
                        d["weights"][d["values"].index(value)] += weight
                    else:
                        d["values"].append(value)
                        d["weights"].append(weight)

    for header, rows in _tables(text):
        low = [h.strip().lower() for h in header]
        if not low or low[0] not in ("dimension", "dim", "factor"):
            continue
        for row in rows:
            if not row or row[0].strip().strip("` ").lower() not in (
                    "λ", "lambda", "stickiness", "λ_d"):
                continue
            for i in range(1, min(len(header), len(row))):
                sym = low[i].strip("` ").upper()[:1]
                if sym not in symbol_to_attr:
                    continue
                try:
                    dims.setdefault(symbol_to_attr[sym], {
                        "values": [], "weights": [], "transitions": {},
                        "stickiness": None, "symbol": sym})["stickiness"] = float(row[i])
                    parsed.append(f"stickiness {symbol_to_attr[sym]}")
                except ValueError:
                    pass

    pending = {}
    for header, rows in _tables(text):
        if not header or "from" not in header[0].lower():
            continue
        matched_attrs = []
        for cell in header[1:]:
            for attr, d in dims.items():
                if _coerce(cell.strip().strip("`"), attr) in d["values"]:
                    matched_attrs.append(attr)
                    break
        if not matched_attrs:
            warnings.append(f"transition table columns not recognised: {header!r}")
            continue
        attr = collections.Counter(matched_attrs).most_common(1)[0][0]
        columns = [_coerce(cell.strip().strip("`"), attr) for cell in header[1:]]
        table = {}
        for row in rows:
            if not row:
                continue
            frm = _coerce(row[0].strip().strip("`"), attr)
            if frm is None:
                continue
            targets = {}
            for to, cell in zip(columns, row[1:]):
                if to is None:
                    continue
                try:
                    targets[to] = max(float(str(cell).strip()), 0.0)
                except ValueError:
                    continue
            if targets:
                table[frm] = targets
        pending.setdefault(attr, {}).update(table)
        parsed.append(f"transition matrix {attr}")
    for attr, table in pending.items():
        d = dims[attr]
        seen = set(table) | {v for row in table.values() for v in row}
        missing = [v for v in seen if v not in d["values"]]
        if missing:
            # The spec's w_d row may omit a value that appears in its own
            # transition matrix (e.g. preset slow). Use the leftover mass.
            remaining = max(0.0, 1.0 - sum(d["weights"]))
            share = remaining / len(missing)
            for v in missing:
                d["values"].append(v)
                d["weights"].append(share)
            warnings.append(
                f"{attr}: value(s) {missing} appear in the transition matrix but not "
                "the w_d row; assigned the remaining stationary weight")
        d["transitions"].update(table)
        total = sum(d["weights"])
        if total > 0:
            d["weights"] = [w / total for w in d["weights"]]

    content_sources = {}
    for header, rows in _tables(text):
        low = [h.strip().lower() for h in header]
        source_i = next((i for i, h in enumerate(low)
                         if "source" in h or h == "lavfi"), None)
        if source_i is None or source_i == 0 or len(header) < 2:
            continue
        for row in rows:
            if source_i >= len(row) or not row[0].strip():
                continue
            label = row[0].strip().strip("`").lower()
            sym = label[:1].upper()
            if sym in code_values and label in code_values[sym]:
                label = str(code_values[sym][label])
            source = row[source_i].strip().strip("`")
            if source and "WxH" in source:
                content_sources[label] = source
                parsed.append(f"lavfi source {label}")

    if not all(a in dims and dims[a]["values"] for a in FACTORED_ORDER):
        return None, warnings, parsed
    model = empty_model()
    model["factored"] = True
    model["dims"] = {a: dims[a] for a in FACTORED_ORDER}
    model["content_sources"] = content_sources
    model["code_values"] = code_values
    return model, warnings, parsed


FALLBACK_GLOBALS = {
    "fps": [(30, 1.0)], "threads": [(6, 1.0)],
    "keyint": [(250, 1.0)], "min_keyint": [(25, 1.0)],
    "scenecut": [(0, 1.0)], "extra_args": [([], 1.0)],
    "resolution": [((640, 360), 1.0)], "frames": [(240, 1.0)],
    "preset": [("medium", 1.0)], "crf": [(23.0, 1.0)],
    "content": [("synthetic", 1.0)],
}


# ---------------------------------------------------------------------------
# Parsing helpers
# ---------------------------------------------------------------------------

def empty_model() -> dict:
    return {"states": [], "initial": None, "transitions": None,
            "global": {}, "global_spec_attrs": set()}


def _split_row(line: str) -> list[str]:
    s = line.strip()
    if s.startswith("|"):
        s = s[1:]
    if s.endswith("|"):
        s = s[:-1]
    return [c.strip() for c in s.split("|")]


def _tables(text: str):
    lines = text.splitlines()
    i = 0
    while i < len(lines) - 1:
        if "|" in lines[i] and _SEP_RE.match(lines[i + 1]):
            header = _split_row(lines[i])
            rows = []
            j = i + 2
            while j < len(lines) and "|" in lines[j] and lines[j].strip().startswith("|"):
                rows.append(_split_row(lines[j]))
                j += 1
            yield header, rows
            i = j
        else:
            i += 1


def _classify(cell: str):
    s = cell.strip().lower().replace("_", "-")
    if s in ("from", "to"):
        return s
    if s in _WEIGHT_ALIASES:
        return "weight"
    if s in _STATE_ALIASES:
        return "state"
    for attr, aliases in ATTR_ALIASES.items():
        if s in aliases or any(s.startswith(a + " ") for a in aliases):
            return attr
    return None


def _weight_and_value(cell: str) -> tuple[str, float]:
    text = cell.strip()
    m = re.match(r"^(.*?)\s*\(([0-9]*\.?[0-9]+)\s*%?\)\s*$", text)
    if not m:
        m = re.match(r"^(.*?)\s*[:=]\s*([0-9]*\.?[0-9]+)\s*%?\s*$", text)
    if not m:
        m = re.match(r"^([0-9]*\.?[0-9]+)\s*%\s+(.+)$", text)
        if m:
            return m.group(2).strip(), float(m.group(1)) / 100.0
    if not m:
        # "640x360 40%" / "ultrafast 0.2" (trailing weight without punctuation)
        m = re.match(r"^(.*?\S)\s+([0-9]*\.?[0-9]+)\s*(%)?\s*$", text)
        if m:
            w = float(m.group(2))
            return m.group(1).strip(), w / 100.0 if m.group(3) else w
    if m:
        w = float(m.group(2))
        if "%" in text and w > 1.0:
            w /= 100.0
        return m.group(1).strip(), w
    return text, 1.0


def _cell_tokens(cell: str) -> list[tuple[str, float]]:
    """A Markdown cell may hold several weighted values separated by , or ;."""
    return [_weight_and_value(part) for part in re.split(r"[,;]", cell) if part.strip()]


def _coerce(value, attr: str):
    if isinstance(value, (list, tuple)) and attr == "resolution" and len(value) >= 2:
        try:
            return (int(value[0]), int(value[1]))
        except (TypeError, ValueError):
            return None
    if isinstance(value, dict):
        for key in ("resolution", "res", "size"):
            if key in value:
                return _coerce(value[key], "resolution")
        if "width" in value and "height" in value:
            try:
                return (int(value["width"]), int(value["height"]))
            except (TypeError, ValueError):
                return None
        return None
    s = str(value).strip()
    if attr == "resolution":
        m = re.search(r"(\d+)\s*[x×]\s*(\d+)", s, re.I)
        return (int(m.group(1)), int(m.group(2))) if m else None
    if attr in ("frames", "fps", "threads", "keyint", "min_keyint", "scenecut"):
        m = _NUM_RE.match(s)
        return int(float(m.group(1))) if m else None
    if attr == "crf":
        m = _NUM_RE.match(s)
        return float(m.group(1)) if m else None
    if attr == "preset":
        return s.lower()
    if attr == "content":
        return s.lower()
    if attr == "extra_args":
        return s.split() if s else []
    return s


def _add_dist(target: dict, attr: str, value, weight: float) -> bool:
    v = _coerce(value, attr)
    if v is None:
        return False
    target.setdefault(attr, []).append((v, max(float(weight), 0.0)))
    return True


def _coerce_dist(value, attr: str, warnings: list) -> list:
    out = []
    if value is None:
        return out
    if isinstance(value, dict):
        items = list(value.items())
    elif isinstance(value, (list, tuple)):
        items = []
        for item in value:
            if isinstance(item, (list, tuple)) and len(item) >= 2:
                items.append((item[0], item[1]))
            elif isinstance(item, dict):
                val = None
                for key in ("value", attr, *ATTR_ALIASES.get(attr, ())):
                    if key in item:
                        val = item[key]
                        break
                w = next((item[k] for k in ("p", "prob", "probability", "weight")
                          if k in item), 1.0)
                items.append((val, w))
            else:
                items.append((item, 1.0))
    else:
        items = [(value, 1.0)]
    for val, w in items:
        v = _coerce(val, attr)
        if v is None:
            warnings.append(f"ignored unusable {attr} value: {val!r}")
            continue
        try:
            weight = float(str(w).rstrip("%")) / (100.0 if str(w).endswith("%") else 1.0)
        except (TypeError, ValueError):
            weight = 1.0
        out.append((v, max(weight, 0.0)))
    return out


def _parse_json_block(obj, model: dict, warnings: list, parsed: list) -> None:
    if not isinstance(obj, dict):
        return
    states = obj.get("states") or obj.get("usage_states") or []
    if isinstance(states, list) and states:
        parsed.append("JSON states")
        for st in states:
            if not isinstance(st, dict):
                continue
            name = str(st.get("name") or st.get("state") or st.get("id") or "state")
            weight = st.get("weight", st.get("prob", st.get("p", 1.0)))
            try:
                weight = float(weight)
            except (TypeError, ValueError):
                weight = 1.0
            entry = {"name": name, "weight": weight, "dist": {}, "spec_attrs": set()}
            for key, val in st.items():
                low = str(key).lower().replace("_", "-")
                attr = _classify(low)
                if attr in (None, "weight", "state", "from", "to"):
                    continue
                dist = _coerce_dist(val, attr, warnings)
                if dist:
                    entry["dist"][attr] = dist
                    entry["spec_attrs"].add(attr)
            model["states"].append(entry)
    # Global distribution: explicit "global" object, or top-level known keys.
    glob = obj.get("global") or obj.get("distribution") or {}
    if not isinstance(glob, dict):
        glob = {}
    for key, val in list(glob.items()) + [
            (k, v) for k, v in obj.items() if _classify(str(k).lower()) in ATTR_ALIASES]:
        attr = _classify(str(key).lower().replace("_", "-"))
        if attr not in ATTR_ALIASES:
            continue
        dist = _coerce_dist(val, attr, warnings)
        if dist:
            model["global"][attr] = dist
            model["global_spec_attrs"].add(attr)
            parsed.append(f"JSON global {attr}")
    initial = obj.get("initial") or obj.get("initial_distribution")
    if isinstance(initial, dict):
        model["initial"] = {str(k): float(v) for k, v in initial.items()
                            if isinstance(v, (int, float))}
        parsed.append("JSON initial distribution")
    transitions = obj.get("transitions")
    if isinstance(transitions, dict):
        model["transitions"] = {
            str(k): {str(k2): float(v2) for k2, v2 in v.items()
                     if isinstance(v2, (int, float))}
            for k, v in transitions.items() if isinstance(v, dict)}
        parsed.append("JSON transitions")


def _parse_table(header: list[str], rows: list[dict], model: dict,
                 warnings: list, parsed: list) -> None:
    cols = [_classify(h) for h in header]
    low = [h.strip().lower() for h in header]
    param_names = ("parameter", "dimension", "variable", "attribute", "field", "factor")
    value_names = ("value", "values", "distribution", "domain")
    param_idx = next((i for i, h in enumerate(low) if h in param_names), None)
    value_idx = next((i for i, h in enumerate(low) if h in value_names), None)
    if (param_idx is not None and value_idx is not None and param_idx != value_idx
            and value_idx < len(header)):
        parsed.append("Markdown parameter/value table")
        for row in rows:
            if param_idx >= len(row) or value_idx >= len(row):
                continue
            attr = _classify(row[param_idx])
            if attr not in ATTR_ALIASES:
                warnings.append(f"ignored unknown usage dimension: {row[param_idx]!r}")
                continue
            ok = False
            for val, w in _cell_tokens(row[value_idx]):
                if _add_dist(model["global"], attr, val, w):
                    ok = True
            if ok:
                model["global_spec_attrs"].add(attr)
        return
    if "from" in low:
        to_indices = [i for i, c in enumerate(cols) if c == "to"]
        if not to_indices:
            to_indices = [i for i in range(1, len(header))]
        if model["transitions"] is None:
            model["transitions"] = {}
        parsed.append("Markdown transition matrix")
        for row in rows:
            if not row or not row[0]:
                continue
            frm = row[0].strip().lower()
            targets = {}
            for i in to_indices:
                if i < len(row):
                    try:
                        targets[header[i].strip().lower()] = float(
                            str(row[i]).strip().rstrip("%")) / (
                                100.0 if str(row[i]).strip().endswith("%") else 1.0)
                    except ValueError:
                        continue
            if targets:
                model["transitions"][frm] = targets
        return

    state_idx = cols.index("state") if "state" in cols else None
    weight_indices = [i for i, c in enumerate(cols) if c == "weight"]
    attr_indices = [i for i, c in enumerate(cols) if c in ATTR_ALIASES]
    if not attr_indices:
        return

    def cell_weight(cell):
        text = cell.strip()
        m = _NUM_RE.match(text)
        if m:
            return float(m.group(1)) / (100.0 if m.group(2) else 1.0)
        _, w = _weight_and_value(text)
        return w

    def row_weight(row):
        if not weight_indices or weight_indices[0] >= len(row):
            return 1.0
        try:
            return cell_weight(row[weight_indices[0]])
        except (TypeError, ValueError):
            return 1.0

    if state_idx is not None:
        parsed.append("Markdown state table")
        by_name = {s["name"]: s for s in model["states"]}
        for row in rows:
            if state_idx >= len(row) or not row[state_idx]:
                continue
            name = row[state_idx].strip().lower()
            if name not in by_name:
                by_name[name] = {"name": name, "weight": 1.0, "dist": {},
                                 "spec_attrs": set()}
                model["states"].append(by_name[name])
            st = by_name[name]
            if weight_indices and weight_indices[0] < len(row):
                try:
                    st["weight"] = cell_weight(row[weight_indices[0]])
                except (TypeError, ValueError):
                    pass
            w = row_weight(row)
            for i in attr_indices:
                if i >= len(row):
                    continue
                ok = False
                for val, local_w in _cell_tokens(row[i]):
                    if _add_dist(st["dist"], cols[i], val, w * local_w):
                        ok = True
                if ok:
                    st["spec_attrs"].add(cols[i])
                    parsed.append(f"state {name} {cols[i]}")
                else:
                    warnings.append(f"ignored unusable {cols[i]} cell: {row[i]!r}")
        if (state_idx is not None and len(attr_indices) == 0 and model["initial"] is None):
            model["initial"] = {s["name"]: s["weight"] for s in model["states"]}
        return

    parsed.append("Markdown global distribution table")
    for row in rows:
        w = row_weight(row)
        for i in attr_indices:
            if i >= len(row):
                continue
            ok = False
            for val, local_w in _cell_tokens(row[i]):
                if _add_dist(model["global"], cols[i], val, w * local_w):
                    ok = True
            if ok:
                model["global_spec_attrs"].add(cols[i])
            else:
                warnings.append(f"ignored unusable {cols[i]} cell: {row[i]!r}")


def _parse_key_values(text: str, model: dict, warnings: list, parsed: list) -> None:
    pattern = re.compile(
        r"^\s*(?:[-*]\s*)?(presets?|resolutions?|frames?|fps|frame[ -]rate|crf|"
        r"quality|content|threads?|keyint|min-keyint|scenecut|extra[ -]args)\s*:\s*(.+?)\s*$",
        re.I)
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith("|") or stripped.startswith("```"):
            continue
        m = pattern.match(line)
        if not m:
            continue
        attr = _classify(m.group(1).strip().lower().replace("_", "-"))
        if attr not in ATTR_ALIASES:
            continue
        for token in re.split(r"[,;]", m.group(2)):
            if not token.strip():
                continue
            val, w = _weight_and_value(token)
            if _add_dist(model["global"], attr, val, w):
                model["global_spec_attrs"].add(attr)
                parsed.append(f"key-value {attr}")


def parse_usage_text(text: str) -> tuple[dict, list[str], list[str]]:
    factored, warnings, parsed = _parse_factored(text)
    if factored is not None:
        return factored, warnings, parsed

    model = empty_model()
    for block in re.findall(r"```(?:json)\s*\n(.*?)\n\s*```", text, re.S | re.I):
        try:
            _parse_json_block(json.loads(block), model, warnings, parsed)
        except json.JSONDecodeError as exc:
            warnings.append(f"JSON code fence ignored: {exc}")
    for header, rows in _tables(text):
        _parse_table(header, rows, model, warnings, parsed)
    _parse_key_values(text, model, warnings, parsed)
    return model, warnings, parsed


# ---------------------------------------------------------------------------
# Loading / sampling
# ---------------------------------------------------------------------------

def _sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def load_usage_model(path: Path, force_fallback: bool = False) -> tuple[dict, dict]:
    """Return (model, provenance).  Never raises on a missing/bad spec file."""
    prov = {
        "path": str(path), "source": None, "fallback_used": False,
        "partial": False, "sha256": None, "warnings": [], "parsed_elements": [],
    }
    if force_fallback:
        prov.update(source="built-in fallback (forced with --force-fallback)",
                    fallback_used=True)
        prov["warnings"].append("Spec sampling disabled by --force-fallback.")
        return builtin_profile(), prov
    if not path.is_file():
        prov.update(source="built-in fallback (spec/usage-model.md absent)",
                    fallback_used=True)
        prov["warnings"].append(
            f"{path} is missing; using documented built-in fallback profile. "
            "Results are NOT drawn from a certified usage model.")
        return builtin_profile(), prov
    try:
        text = path.read_text(encoding="utf-8")
        prov["sha256"] = _sha256(path)
    except (OSError, UnicodeDecodeError) as exc:
        prov.update(source="built-in fallback (spec unreadable)", fallback_used=True)
        prov["warnings"].append(f"Cannot read {path}: {exc}; using built-in fallback.")
        return builtin_profile(), prov

    model, warnings, parsed = parse_usage_text(text)
    prov["warnings"].extend(warnings)
    prov["parsed_elements"] = sorted(set(parsed))
    if model.get("factored"):
        prov.update(source=str(path), fallback_used=False,
                    model_kind="factored first-order Markov chain (spec §2)")
        prov["states"] = [f"{a}: {len(d['values'])} values"
                          for a, d in model["dims"].items()]
        prov["initial"] = {a: [round(w, 4) for w in d["weights"]]
                           for a, d in model["dims"].items()}
        prov["transitions"] = {a: len(d["transitions"])
                               for a, d in model["dims"].items()}
        prov["content_sources"] = model.get("content_sources", {})
        return model, prov
    has_states = bool(model["states"])
    has_global = any(a in model["global"] for a in ("resolution", "frames", "preset"))
    if not has_states and not has_global:
        prov.update(source="built-in fallback (spec had no usable distributions)",
                    fallback_used=True)
        prov["warnings"].append(
            f"{path} contained no usable resolution/frames/preset distribution; "
            "using documented built-in fallback profile.")
        return builtin_profile(), prov

    # Merge the fallback into missing attributes so sampling is always total.
    missing = []
    if not has_states:
        model["states"] = [{"name": "spec", "weight": 1.0, "dist": {},
                            "spec_attrs": set(model["global_spec_attrs"])}]
        if model["initial"] is None:
            model["initial"] = {"spec": 1.0}
    for st in model["states"]:
        for attr, dist in FALLBACK_GLOBALS.items():
            if attr not in st["dist"]:
                if attr in model["global"]:
                    st["dist"][attr] = copy.deepcopy(model["global"][attr])
                    if attr in model["global_spec_attrs"]:
                        st["spec_attrs"].add(attr)
                else:
                    st["dist"][attr] = copy.deepcopy(dist)
                    missing.append(f"{st['name']}.{attr}")
        st["weight"] = max(float(st.get("weight", 1.0)), 0.0)
    if model["initial"] is None:
        model["initial"] = {s["name"]: s["weight"] for s in model["states"]}
    if missing:
        prov["partial"] = True
        prov["warnings"].append(
            "Attributes filled from built-in defaults (not from the spec): "
            + ", ".join(sorted(set(missing))))

    prov.update(source=str(path), fallback_used=False)
    prov["states"] = [s["name"] for s in model["states"]]
    prov["initial"] = model["initial"]
    prov["transitions"] = model["transitions"]
    return model, prov


def _choose(dist: list, rng):
    if not dist:
        return None
    values = [d[0] for d in dist]
    weights = [max(float(d[1]), 0.0) for d in dist]
    if sum(weights) <= 0:
        weights = [1.0] * len(values)
    return rng.choices(values, weights=weights, k=1)[0]


def sample_factored_cases(model: dict, n: int, rng, seed: int,
                          presets_filter=None, frames_cap=None):
    """Sample the factored chain of spec/usage-model.md §2."""
    dims = model["dims"]
    warnings = []
    prev = {}
    cases = []
    for i in range(n):
        values = {}
        for attr in FACTORED_ORDER:
            d = dims[attr]
            dist = list(zip(d["values"], d["weights"]))
            if attr == "preset" and presets_filter:
                dist = [pair for pair in dist if pair[0] in presets_filter]
                if not dist:
                    raise ValueError(
                        f"--presets {presets_filter} matches no preset in the usage model")
            row = d["transitions"].get(prev.get(attr)) if prev else None
            if row is None and prev and d.get("stickiness") is not None:
                lam = d["stickiness"]
                row = {v: (lam if v == prev[attr] else (1.0 - lam) * w)
                       for v, w in zip(d["values"], d["weights"])}
            if row:
                row_dist = [(v, row.get(v, 0.0)) for v in d["values"]]
                if attr == "preset" and presets_filter:
                    row_dist = [(v, w) for v, w in row_dist if v in presets_filter]
                if sum(w for _, w in row_dist) > 0:
                    dist = row_dist
            value = _choose(dist, rng)
            if value is None:
                raise ValueError(f"no usable value for dimension {attr}")
            values[attr] = value
        frames = int(values["frames"])
        if frames_cap is not None:
            frames = min(frames, int(frames_cap))
        resolution = values["resolution"]
        from_spec = {a: "spec" for a in FACTORED_ORDER}
        for a in ("fps", "keyint", "min_keyint", "scenecut", "extra_args"):
            from_spec[a] = "built-in default (spec §4: use same GOP defaults)"
        cases.append({
            "case_id": f"case-{i + 1:03d}",
            "state": " ".join(f"{dims[a]['symbol']}={values[a]}" for a in FACTORED_ORDER),
            "width": int(resolution[0]), "height": int(resolution[1]),
            "frames": frames,
            "fps": 30,  # spec §1: "Frame count at 30 fps"
            "preset": values["preset"],
            "crf": float(values["crf"]),
            "content": str(values["content"]),
            "seed": int(seed) + i * 104729,
            "threads": int(values["threads"]),
            "keyint": 250, "min_keyint": 25, "scenecut": 0,
            "extra_args": [],
            "attribute_sources": from_spec,
        })
        prev = values
    return cases, warnings


def sample_cases(model: dict, n: int, rng, seed: int,
                 presets_filter=None, frames_cap=None) -> tuple[list[dict], list[str]]:
    if model.get("factored"):
        return sample_factored_cases(model, n, rng, seed, presets_filter, frames_cap)
    states = model["states"]
    warnings = []
    initial = model.get("initial") or {s["name"]: s.get("weight", 1.0) for s in states}
    initial_dist = [(name, float(w)) for name, w in initial.items()]
    transitions = model.get("transitions") or {}
    names = [s["name"] for s in states]
    by_name = {s["name"]: s for s in states}
    state = _choose(initial_dist, rng) or names[0]

    cases = []
    for i in range(n):
        if i > 0:
            nxt = transitions.get(state)
            state = _choose([(k, float(v)) for k, v in nxt.items()], rng) if nxt else None
            if state not in by_name:
                state = _choose(initial_dist, rng) or names[0]
        st = by_name[state]
        dist = st["dist"]
        spec_attrs = set(st.get("spec_attrs", ()))
        resolution = _choose(dist.get("resolution", []), rng) or (640, 360)
        frames = _choose(dist.get("frames", []), rng) or 240
        if frames_cap is not None:
            frames = min(int(frames), int(frames_cap))
        preset = _choose(dist.get("preset", []), rng) or "medium"
        if presets_filter:
            allowed = [d for d in dist.get("preset", []) if d[0] in presets_filter]
            if not allowed:
                # This state never emits a requested preset: use the union of
                # allowed presets across states rather than silently ignoring
                # the filter or aborting the whole run.
                allowed = [d for st in states for d in st["dist"].get("preset", [])
                           if d[0] in presets_filter]
                if not allowed:
                    raise ValueError(
                        f"--presets {presets_filter} matches no preset in the usage model")
                warnings.append(
                    f"state {state} has no preset in --presets {presets_filter}; "
                    "used the model-wide allowed preset distribution instead")
            preset = _choose(allowed, rng)
        case = {
            "case_id": f"case-{i + 1:03d}",
            "state": state,
            "width": int(resolution[0]), "height": int(resolution[1]),
            "frames": int(frames),
            "fps": int(_choose(dist.get("fps", []), rng) or 30),
            "preset": preset,
            "crf": float(_choose(dist.get("crf", []), rng) or 23.0),
            "content": str(_choose(dist.get("content", []), rng) or "synthetic"),
            "seed": int(seed) + i * 104729,
            "threads": int(_choose(dist.get("threads", []), rng) or 6),
            "keyint": int(_choose(dist.get("keyint", []), rng) or 250),
            "min_keyint": int(_choose(dist.get("min_keyint", []), rng) or 25),
            "scenecut": int(_choose(dist.get("scenecut", []), rng) or 0),
            "extra_args": list(_choose(dist.get("extra_args", []), rng) or []),
            "attribute_sources": {
                a: ("spec" if a in spec_attrs else "built-in default")
                for a in ("resolution", "frames", "fps", "preset", "crf", "content",
                          "threads", "keyint", "min_keyint", "scenecut", "extra_args")
            },
        }
        cases.append(case)
    if any(a == "built-in default" for c in cases
           for a in c["attribute_sources"].values()):
        warnings.append("Some case attributes came from built-in defaults, not the spec.")
    return cases, warnings
