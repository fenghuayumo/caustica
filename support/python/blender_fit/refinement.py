"""Deterministic, bounded geometry proposals; no cameras or validation feedback."""
import copy

from .common import validate_geometry


FIELDS = ("size", "position", "rotation", "profile")


def dimensions(part):
    if part["kind"] != "lathe":
        return list(part["size"])
    radius = max(s[1] for s in part["profile"])
    height = part["profile"][-1][0] - part["profile"][0][0]
    return [2 * radius * part["size"][0], 2 * radius * part["size"][1], height * part["size"][2]]


def moves(geometry, fields=FIELDS):
    """Interleave parts so one part cannot consume the entire search budget."""
    for field in fields:
        count = max(len(p.get("profile", [])) for p in geometry["parts"]) if field == "profile" else 3
        for axis in range(count):
            for pi, part in enumerate(geometry["parts"]):
                if field == "profile" and (part["kind"] != "lathe" or axis >= len(part["profile"])):
                    continue
                for sign in (-1, 1):
                    yield pi, field, axis, sign


def propose(geometry, move, level):
    """Position step follows physical extent, not a lathe's dimensionless size."""
    candidate = copy.deepcopy(geometry)
    pi, field, axis, sign = move
    part = candidate["parts"][pi]
    factor = 0.5 ** level
    if field == "size":
        part[field][axis] *= 1 + sign * 0.2 * factor
    elif field == "position":
        part[field][axis] += sign * 0.1 * max(dimensions(part)) * factor
    elif field == "rotation":
        part[field][axis] += sign * 10 * factor
    else:
        # Radius only: preserve increasing heights and open-shell validity.
        part["profile"][axis][1] *= 1 + sign * 0.2 * factor
    try:
        validate_geometry(candidate)
    except ValueError:
        return None
    return candidate


def refine(get_best, evaluate, budget, *, fields=FIELDS, start_level=0):
    """Spend budget in complete sweeps, then reduce steps, up to six levels.

    An accepted move is the base for subsequent proposals. The callback owns
    rendering, selection and checkpointing. Invalid shells cost no renders.
    """
    used = 0
    for level in range(start_level, start_level + 6):
        for move in moves(get_best(), fields):
            if used >= budget:
                return used
            candidate = propose(get_best(), move, level)
            if candidate is None:
                continue
            pi, field, axis, sign = move
            evaluate(candidate, f"{field}/{pi}/{axis}/{sign}/level-{level}")
            used += 1
    return used
