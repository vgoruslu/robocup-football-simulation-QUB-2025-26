#!/usr/bin/env python3
from __future__ import annotations

import argparse
import base64
import csv
import html
import json
import math
import re
import statistics
import zipfile
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

# Batch analyser for RoboCup match logs.
# Produces per-match stats, aggregate team/player metrics, visualisations,
# and a self-contained HTML report for review.

FIELD_X_MIN, FIELD_X_MAX = -52.5, 52.5
FIELD_Y_MIN, FIELD_Y_MAX = -34.0, 34.0
PLAY_ON = "play_on"

BOX_X = 36.0
BOX_Y = 20.16
THIRD_LINE = 17.5

BALL_RE = re.compile(r"\(\(b\)\s+([-\d.eE]+)\s+([-\d.eE]+)\s+([-\d.eE]+)\s+([-\d.eE]+)\)")

# Regex for extracting the player state fields used by the analysis pipeline.
PLAYER_RE = re.compile(
    r"\(\(([lr])\s+(\d+)\)\s+"
    r"(\d+)\s+"
    r"([^\s]+)\s+"
    r"([-\d.eE]+)\s+([-\d.eE]+)\s+"
    r"([-\d.eE]+)\s+([-\d.eE]+)\s+"
    r"([-\d.eE]+)\s+([-\d.eE]+)\s+"
    r"\(v [^)]*\)\s+"
    r"\(fp [^)]*\)\s+"
    r"\(s\s+([-\d.eE]+)\s+([-\d.eE]+)\s+([-\d.eE]+)\s+([-\d.eE]+)\)"
)

# Fields that should not be summed during aggregation.
NON_ADDITIVE_FIELDS = {
    "estimated_possession_pct",
    "pass_completion_pct_est",
    "game",
    "opponent",
    "scoreline",
    "result",
}


def safe_name(text: str) -> str:
    return re.sub(r"[^A-Za-z0-9._-]+", "_", text).strip("_") or "team"


def save_csv(path: Path, rows: List[dict]) -> None:
    if not rows:
        path.write_text("", encoding="utf-8")
        return

    fieldnames: List[str] = []
    seen = set()
    for row in rows:
        for key in row.keys():
            if key not in seen:
                seen.add(key)
                fieldnames.append(key)

    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def save_json(path: Path, obj: Any) -> None:
    path.write_text(json.dumps(obj, indent=2), encoding="utf-8")


def round_if_float(value: Any, digits: int = 2) -> Any:
    if isinstance(value, float):
        return round(value, digits)
    return value


def round_rows(rows: List[dict], digits: int = 2) -> List[dict]:
    return [{k: round_if_float(v, digits) for k, v in row.items()} for row in rows]


def reorder_rows(rows: List[dict], preferred_order: List[str]) -> List[dict]:
    out = []
    for row in rows:
        new_row = {}
        for key in preferred_order:
            if key in row:
                new_row[key] = row[key]
        for key, value in row.items():
            if key not in new_row:
                new_row[key] = value
        out.append(new_row)
    return out


@dataclass
class PlayerFrame:
    side: str
    unum: int
    x: float
    y: float
    vx: float
    vy: float
    body: float
    neck: float
    stamina: float
    effort: float
    recovery: float
    stamina_capacity: float


@dataclass
class Frame:
    cycle: int
    play_mode: str
    ball_x: float
    ball_y: float
    ball_vx: float
    ball_vy: float
    players: Dict[Tuple[str, int], PlayerFrame] = field(default_factory=dict)


@dataclass
class TouchEvent:
    cycle: int
    side: str
    unum: int


class RCGParser:
    def __init__(self, rcg_path: Path):
        self.rcg_path = rcg_path
        self.team_names: Dict[str, str] = {}
        self.current_play_mode = "unknown"
        self.frames: List[Frame] = []
        self.score_by_cycle: Dict[int, Tuple[int, int]] = {}
        self.playmode_changes: List[Tuple[int, str]] = []

    def parse(self) -> "RCGParser":
        with self.rcg_path.open("r", encoding="utf-8", errors="ignore") as f:
            for raw in f:
                line = raw.strip()
                if not line or line == "ULG6":
                    continue

                if line.startswith("(team "):
                    parts = line.strip("()\n").split()
                    if len(parts) >= 6:
                        _, _, left_name, right_name, left_score, right_score = parts[:6]
                        self.team_names["l"] = left_name
                        self.team_names["r"] = right_name
                        score_cycle = self.frames[-1].cycle if self.frames else 0
                        self.score_by_cycle[score_cycle] = (int(left_score), int(right_score))
                    continue

                if line.startswith("(playmode "):
                    parts = line.strip("()\n").split(maxsplit=2)
                    if len(parts) >= 3:
                        _, cycle_s, mode = parts
                        self.current_play_mode = mode
                        try:
                            self.playmode_changes.append((int(cycle_s), mode))
                        except ValueError:
                            pass
                    continue

                if line.startswith("(show "):
                    frame = self._parse_show_line(line)
                    if frame is not None:
                        self.frames.append(frame)

        return self

    def _parse_show_line(self, line: str) -> Optional[Frame]:
        m_cycle = re.match(r"^\(show\s+(\d+)\s+", line)
        if not m_cycle:
            return None

        cycle = int(m_cycle.group(1))
        m_ball = BALL_RE.search(line)
        if not m_ball:
            return None

        ball_x, ball_y, ball_vx, ball_vy = map(float, m_ball.groups())
        players: Dict[Tuple[str, int], PlayerFrame] = {}

        for pm in PLAYER_RE.finditer(line):
            side, unum, _ptype, _flags, x, y, vx, vy, body, neck, stamina, effort, recovery, capacity = pm.groups()
            players[(side, int(unum))] = PlayerFrame(
                side=side,
                unum=int(unum),
                x=float(x),
                y=float(y),
                vx=float(vx),
                vy=float(vy),
                body=float(body),
                neck=float(neck),
                stamina=float(stamina),
                effort=float(effort),
                recovery=float(recovery),
                stamina_capacity=float(capacity),
            )

        return Frame(cycle, self.current_play_mode, ball_x, ball_y, ball_vx, ball_vy, players)


def flip_playmode(mode: str) -> str:
    if "goal_l" in mode:
        return mode.replace("goal_l", "goal_r")
    if "goal_r" in mode:
        return mode.replace("goal_r", "goal_l")
    if mode.endswith("_l"):
        return mode[:-2] + "_r"
    if mode.endswith("_r"):
        return mode[:-2] + "_l"
    if "_l_" in mode:
        return mode.replace("_l_", "_TMP_").replace("_r_", "_l_").replace("_TMP_", "_r_")
    return mode


def normalize_match_to_reference(
    parser: RCGParser,
    reference_team: str,
) -> Tuple[List[Frame], Dict[str, str], Dict[int, Tuple[int, int]], List[Tuple[int, str]], bool]:
    left_name = parser.team_names.get("l")
    right_name = parser.team_names.get("r")

    if left_name != reference_team and right_name != reference_team:
        raise ValueError(
            f"Reference team '{reference_team}' not found in file {parser.rcg_path.name}. "
            f"Found teams: left={left_name}, right={right_name}"
        )

    # Normalise the reference team onto a consistent side so outputs are comparable across matches.
    flipped = left_name == reference_team

    if not flipped:
        return (
            parser.frames,
            dict(parser.team_names),
            dict(parser.score_by_cycle),
            list(parser.playmode_changes),
            False,
        )

    normalized_frames: List[Frame] = []
    for fr in parser.frames:
        new_players: Dict[Tuple[str, int], PlayerFrame] = {}
        for (side, unum), p in fr.players.items():
            new_side = "r" if side == "l" else "l"
            new_players[(new_side, unum)] = PlayerFrame(
                side=new_side,
                unum=unum,
                x=-p.x,
                y=-p.y,
                vx=-p.vx,
                vy=-p.vy,
                body=(p.body + 180.0) % 360.0,
                neck=p.neck,
                stamina=p.stamina,
                effort=p.effort,
                recovery=p.recovery,
                stamina_capacity=p.stamina_capacity,
            )

        normalized_frames.append(
            Frame(
                cycle=fr.cycle,
                play_mode=flip_playmode(fr.play_mode),
                ball_x=-fr.ball_x,
                ball_y=-fr.ball_y,
                ball_vx=-fr.ball_vx,
                ball_vy=-fr.ball_vy,
                players=new_players,
            )
        )

    normalized_team_names = {"l": right_name, "r": left_name}
    normalized_scores = {cyc: (r_score, l_score) for cyc, (l_score, r_score) in parser.score_by_cycle.items()}
    normalized_playmodes = [(cyc, flip_playmode(mode)) for cyc, mode in parser.playmode_changes]

    return normalized_frames, normalized_team_names, normalized_scores, normalized_playmodes, True


class Analyzer:
    def __init__(
        self,
        frames: List[Frame],
        team_names: Dict[str, str],
        score_by_cycle: Dict[int, Tuple[int, int]],
        playmode_changes: List[Tuple[int, str]],
        exclude_cycles_after_restart: int = 15,
    ):
        self.frames = frames
        self.team_names = {k: team_names.get(k, k) for k in ("l", "r")}
        self.score_by_cycle = score_by_cycle
        self.playmode_changes = playmode_changes
        self.exclude_cycles_after_restart = exclude_cycles_after_restart

        self.play_on_frames = [f for f in frames if f.play_mode == PLAY_ON]
        self.tactical_frames = self._filter_tactical_frames()
        self.touch_events = self._build_touch_events()
        self.goals, self.assists = self._estimate_goals_and_assists()
        self.pass_attempts, self.passes_completed = self._estimate_passes()
        self.stamina_usage = self._estimate_stamina_usage()
        self.distance_travelled = self._estimate_distance_travelled()

    def _filter_tactical_frames(self) -> List[Frame]:
        # Ignore restart transitions when building tactical views; set pieces can skew average shape.
        out: List[Frame] = []
        idx = 0
        prev_mode = None

        for fr in self.frames:
            if fr.play_mode != PLAY_ON:
                idx = 0
                prev_mode = fr.play_mode
                continue

            if prev_mode != PLAY_ON:
                idx = 0

            prev_mode = fr.play_mode
            if idx >= self.exclude_cycles_after_restart:
                out.append(fr)
            idx += 1

        return out

    def final_score(self) -> Tuple[int, int]:
        return self.score_by_cycle[max(self.score_by_cycle)] if self.score_by_cycle else (0, 0)

    def nearest_player(self, fr: Frame, max_dist: float = 1.3) -> Optional[Tuple[str, int]]:
        # Possession is approximated from the nearest player to the ball.
        best = None
        for (side, unum), p in fr.players.items():
            d = math.hypot(p.x - fr.ball_x, p.y - fr.ball_y)
            if best is None or d < best[0]:
                best = (d, side, unum)
        if best and best[0] <= max_dist:
            return best[1], best[2]
        return None

    def _build_touch_events(self) -> List[TouchEvent]:
        events: List[TouchEvent] = []
        prev_owner = None
        for fr in self.play_on_frames:
            owner = self.nearest_player(fr)
            if owner is None:
                continue
            if owner != prev_owner:
                events.append(TouchEvent(fr.cycle, owner[0], owner[1]))
                prev_owner = owner
        return events

    def _event_before_cycle(
        self,
        cycle: int,
        side: Optional[str] = None,
        distinct_from: Optional[Tuple[str, int]] = None,
        max_lookback: int = 120,
    ) -> Optional[TouchEvent]:
        for ev in reversed(self.touch_events):
            if ev.cycle >= cycle:
                continue
            if cycle - ev.cycle > max_lookback:
                break
            if side is not None and ev.side != side:
                continue
            if distinct_from is not None and (ev.side, ev.unum) == distinct_from:
                continue
            return ev
        return None

    def _estimate_goals_and_assists(self) -> Tuple[Counter, Counter]:
        # Attribute goals and assists from the last relevant touches before each goal event.
        goals = Counter()
        assists = Counter()

        for cycle, mode in self.playmode_changes:
            if mode.startswith("goal_l"):
                scoring_side = "l"
            elif mode.startswith("goal_r"):
                scoring_side = "r"
            else:
                continue

            scorer = self._event_before_cycle(cycle, side=scoring_side, max_lookback=120)
            if scorer:
                goals[(scoring_side, scorer.unum)] += 1
                assister = self._event_before_cycle(
                    cycle=scorer.cycle,
                    side=scoring_side,
                    distinct_from=(scoring_side, scorer.unum),
                    max_lookback=120,
                )
                if assister:
                    assists[(scoring_side, assister.unum)] += 1

        return goals, assists

    def _estimate_passes(self) -> Tuple[Counter, Counter]:
        # Treat a touch-to-touch transition as a pass attempt within the configured time window.
        attempts = Counter()
        completed = Counter()

        for a, b in zip(self.touch_events, self.touch_events[1:]):
            gap = b.cycle - a.cycle
            if gap <= 0 or gap > 80:
                continue

            attempts[(a.side, a.unum)] += 1
            if a.side == b.side and a.unum != b.unum:
                completed[(a.side, a.unum)] += 1

        return attempts, completed

    def _estimate_stamina_usage(self) -> Dict[Tuple[str, int], float]:
        # Track stamina consumption as cumulative drops, allowing for in-game recovery.
        totals = defaultdict(float)
        prev_stamina = {}
        prev_cycle = {}

        for fr in self.frames:
            for key, p in fr.players.items():
                if key in prev_stamina and fr.cycle - prev_cycle[key] <= 2:
                    drop = prev_stamina[key] - p.stamina
                    if drop > 0:
                        totals[key] += drop
                prev_stamina[key] = p.stamina
                prev_cycle[key] = fr.cycle

        return {k: round(v, 2) for k, v in totals.items()}

    def _estimate_distance_travelled(self) -> Dict[Tuple[str, int], float]:
        totals = defaultdict(float)
        prev_pos = {}
        prev_cycle = {}

        for fr in self.play_on_frames:
            for key, p in fr.players.items():
                if key in prev_pos and fr.cycle - prev_cycle[key] <= 2:
                    totals[key] += math.hypot(p.x - prev_pos[key][0], p.y - prev_pos[key][1])
                prev_pos[key] = (p.x, p.y)
                prev_cycle[key] = fr.cycle

        return {k: round(v, 2) for k, v in totals.items()}

    def possession_pct(self) -> Dict[str, float]:
        counts = Counter()
        for fr in self.play_on_frames:
            owner = self.nearest_player(fr)
            if owner:
                counts[owner[0]] += 1

        total = counts["l"] + counts["r"]
        if total == 0:
            return {"l": 0.0, "r": 0.0}

        return {
            "l": round(100.0 * counts["l"] / total, 2),
            "r": round(100.0 * counts["r"] / total, 2),
        }

    def team_ball_cycles(self) -> List[dict]:
        rows: List[dict] = []
        score = self.final_score()
        possession = self.possession_pct()
        play_on_cycles = len(self.play_on_frames)

        for side in ("l", "r"):
            own_half = opp_half = own_final = opp_final = own_box = opp_box = middle = 0

            for fr in self.play_on_frames:
                x, y = fr.ball_x, fr.ball_y

                if side == "l":
                    in_own_half = x < 0
                    in_opp_half = x >= 0
                    in_own_final = x < -THIRD_LINE
                    in_opp_final = x > THIRD_LINE
                    in_own_box = x <= -BOX_X and abs(y) <= BOX_Y
                    in_opp_box = x >= BOX_X and abs(y) <= BOX_Y
                else:
                    in_own_half = x > 0
                    in_opp_half = x <= 0
                    in_own_final = x > THIRD_LINE
                    in_opp_final = x < -THIRD_LINE
                    in_own_box = x >= BOX_X and abs(y) <= BOX_Y
                    in_opp_box = x <= -BOX_X and abs(y) <= BOX_Y

                own_half += int(in_own_half)
                opp_half += int(in_opp_half)
                own_final += int(in_own_final)
                opp_final += int(in_opp_final)
                own_box += int(in_own_box)
                opp_box += int(in_opp_box)
                middle += int(-THIRD_LINE <= x <= THIRD_LINE)

            goals_for = score[0] if side == "l" else score[1]
            goals_against = score[1] if side == "l" else score[0]

            rows.append(
                {
                    "team": self.team_names[side],
                    "goals_scored": goals_for,
                    "goals_conceded": goals_against,
                    "goal_difference": goals_for - goals_against,
                    "estimated_possession_pct": possession[side],
                    "play_on_cycles": play_on_cycles,
                    "ball_cycles_in_own_half": own_half,
                    "ball_cycles_in_opponent_half": opp_half,
                    "ball_cycles_in_middle_third": middle,
                    "ball_cycles_in_own_final_third": own_final,
                    "ball_cycles_in_opponent_final_third": opp_final,
                    "ball_cycles_in_own_box": own_box,
                    "ball_cycles_in_opponent_box": opp_box,
                }
            )

        return rows

    def player_rows(self) -> List[dict]:
        keys = sorted({k for fr in self.frames for k in fr.players})
        rows: List[dict] = []

        for side, unum in keys:
            att = self.pass_attempts.get((side, unum), 0)
            comp = self.passes_completed.get((side, unum), 0)
            pct = round(100.0 * comp / att, 2) if att else 0.0

            rows.append(
                {
                    "team": self.team_names[side],
                    "player": unum,
                    "goals": self.goals.get((side, unum), 0),
                    "estimated_assists": self.assists.get((side, unum), 0),
                    "pass_attempts_est": att,
                    "passes_completed_est": comp,
                    "pass_completion_pct_est": pct,
                    "total_stamina_used": self.stamina_usage.get((side, unum), 0.0),
                    "distance_travelled_play_on": self.distance_travelled.get((side, unum), 0.0),
                }
            )

        return rows


def aggregate_rows(rows: List[dict], key_fields: List[str], average_divisor: int) -> Tuple[List[dict], List[dict]]:
    totals: Dict[Tuple[Any, ...], dict] = {}

    for row in rows:
        key = tuple(row[k] for k in key_fields)
        if key not in totals:
            totals[key] = {k: row[k] for k in key_fields}

        for k, v in row.items():
            if k in key_fields or k in NON_ADDITIVE_FIELDS:
                continue
            if isinstance(v, (int, float)) and not isinstance(v, bool):
                totals[key][k] = totals[key].get(k, 0) + v

    total_rows = list(totals.values())
    avg_rows = []

    for row in total_rows:
        avg_row = {k: row[k] for k in key_fields}
        for k, v in row.items():
            if k in key_fields:
                continue
            if isinstance(v, (int, float)) and not isinstance(v, bool):
                avg_row[k] = round(v / average_divisor, 4)
        avg_rows.append(avg_row)

    total_rows.sort(key=lambda r: tuple(r[k] for k in key_fields))
    avg_rows.sort(key=lambda r: tuple(r[k] for k in key_fields))
    return total_rows, avg_rows


def draw_pitch(ax) -> None:
    ax.set_xlim(FIELD_X_MIN, FIELD_X_MAX)
    ax.set_ylim(FIELD_Y_MIN, FIELD_Y_MAX)
    ax.set_aspect("equal")
    ax.plot(
        [FIELD_X_MIN, FIELD_X_MAX, FIELD_X_MAX, FIELD_X_MIN, FIELD_X_MIN],
        [FIELD_Y_MIN, FIELD_Y_MIN, FIELD_Y_MAX, FIELD_Y_MAX, FIELD_Y_MIN],
        linewidth=1,
    )
    ax.axvline(0, linewidth=1)
    ax.add_patch(plt.Circle((0, 0), 9.15, fill=False, linewidth=1))
    ax.add_patch(plt.Rectangle((FIELD_X_MIN, -20.16), 16.5, 40.32, fill=False, linewidth=1))
    ax.add_patch(plt.Rectangle((FIELD_X_MAX - 16.5, -20.16), 16.5, 40.32, fill=False, linewidth=1))
    ax.set_xticks([])
    ax.set_yticks([])


def make_ball_heatmap(analyzer: Analyzer, out_dir: Path, suffix: str = "") -> Path:
    fig, ax = plt.subplots(figsize=(10, 6.5))
    draw_pitch(ax)
    xs = [f.ball_x for f in analyzer.play_on_frames]
    ys = [f.ball_y for f in analyzer.play_on_frames]
    if xs:
        ax.hist2d(xs, ys, bins=[36, 24], range=[[FIELD_X_MIN, FIELD_X_MAX], [FIELD_Y_MIN, FIELD_Y_MAX]])
    ax.set_title("Ball heatmap (play_on only)")
    path = out_dir / f"ball_heatmap{suffix}.png"
    fig.savefig(path, dpi=160, bbox_inches="tight")
    plt.close(fig)
    return path


def make_average_formation(analyzer: Analyzer, out_dir: Path, side: str, suffix: str = "") -> Path:
    frames = analyzer.tactical_frames or analyzer.play_on_frames
    pts = defaultdict(lambda: [[], []])

    for fr in frames:
        for (s, unum), p in fr.players.items():
            if s == side:
                pts[unum][0].append(p.x)
                pts[unum][1].append(p.y)

    fig, ax = plt.subplots(figsize=(10, 6.5))
    draw_pitch(ax)

    for unum, (xs, ys) in sorted(pts.items()):
        if xs:
            x = statistics.fmean(xs)
            y = statistics.fmean(ys)
            ax.scatter([x], [y], s=120)
            ax.text(x, y, str(unum), ha="center", va="center", fontsize=9)

    ax.set_title(f"{analyzer.team_names[side]} average formation (play_on only)")
    path = out_dir / f"average_formation_{safe_name(analyzer.team_names[side])}{suffix}.png"
    fig.savefig(path, dpi=160, bbox_inches="tight")
    plt.close(fig)
    return path


def make_team_heatmap(analyzer: Analyzer, out_dir: Path, side: str, suffix: str = "") -> Path:
    frames = analyzer.tactical_frames or analyzer.play_on_frames
    xs, ys = [], []

    for fr in frames:
        for (s, _), p in fr.players.items():
            if s == side:
                xs.append(p.x)
                ys.append(p.y)

    fig, ax = plt.subplots(figsize=(10, 6.5))
    draw_pitch(ax)
    if xs:
        ax.hist2d(xs, ys, bins=[36, 24], range=[[FIELD_X_MIN, FIELD_X_MAX], [FIELD_Y_MIN, FIELD_Y_MAX]])
    ax.set_title(f"{analyzer.team_names[side]} team heatmap (play_on only)")
    path = out_dir / f"team_heatmap_{safe_name(analyzer.team_names[side])}{suffix}.png"
    fig.savefig(path, dpi=160, bbox_inches="tight")
    plt.close(fig)
    return path


def make_player_heatmaps(analyzer: Analyzer, out_dir: Path, side: str, suffix: str = "") -> List[Path]:
    frames = analyzer.tactical_frames or analyzer.play_on_frames
    paths = []
    unums = sorted({unum for fr in frames for (s, unum) in fr.players if s == side})

    for unum in unums:
        xs, ys = [], []
        for fr in frames:
            p = fr.players.get((side, unum))
            if p:
                xs.append(p.x)
                ys.append(p.y)

        fig, ax = plt.subplots(figsize=(9, 5.7))
        draw_pitch(ax)
        if xs:
            ax.hist2d(xs, ys, bins=[32, 20], range=[[FIELD_X_MIN, FIELD_X_MAX], [FIELD_Y_MIN, FIELD_Y_MAX]])
        ax.set_title(f"{analyzer.team_names[side]} #{unum} heatmap")
        path = out_dir / f"player_heatmap_{safe_name(analyzer.team_names[side])}_{unum}{suffix}.png"
        fig.savefig(path, dpi=150, bbox_inches="tight")
        plt.close(fig)
        paths.append(path)

    return paths


def make_team_zone_bars_from_rows(team_rows: List[dict], out_dir: Path, suffix: str = "") -> Path:
    rows = sorted(team_rows, key=lambda r: r["team"])
    fig, ax = plt.subplots(figsize=(13, 5.4))

    if len(rows) >= 2:
        categories = [
            "ball_cycles_in_own_half",
            "ball_cycles_in_opponent_half",
            "ball_cycles_in_middle_third",
            "ball_cycles_in_own_final_third",
            "ball_cycles_in_opponent_final_third",
            "ball_cycles_in_own_box",
            "ball_cycles_in_opponent_box",
        ]
        labels = [
            "Own half",
            "Opponent half",
            "Middle third",
            "Own final third",
            "Opponent final third",
            "Own box",
            "Opponent box",
        ]

        x = list(range(len(categories)))
        width = 0.35
        vals1 = [rows[0].get(c, 0) for c in categories]
        vals2 = [rows[1].get(c, 0) for c in categories]

        ax.bar([i - width / 2 for i in x], vals1, width=width, label=rows[0]["team"])
        ax.bar([i + width / 2 for i in x], vals2, width=width, label=rows[1]["team"])
        ax.set_xticks(x)
        ax.set_xticklabels(labels, rotation=18, ha="right")
        ax.legend()
    else:
        ax.text(0.5, 0.5, "Not enough team data for chart", ha="center", va="center", transform=ax.transAxes)
        ax.set_xticks([])
        ax.set_yticks([])

    ax.set_ylabel("Average cycles per game")
    ax.set_title("Average team relative ball cycles (play_on only)")

    path = out_dir / f"team_relative_ball_cycles{suffix}.png"
    fig.savefig(path, dpi=160, bbox_inches="tight")
    plt.close(fig)
    return path


def make_possession_barchart_from_rows(team_rows: List[dict], out_dir: Path, suffix: str = "") -> Path:
    rows = sorted(team_rows, key=lambda r: r["team"])
    fig, ax = plt.subplots(figsize=(8, 4.8))

    if rows:
        teams = [row["team"] for row in rows]
        vals = [row.get("estimated_possession_pct", 0) for row in rows]
        bars = ax.bar(teams, vals)
        for bar, val in zip(bars, vals):
            ax.text(
                bar.get_x() + bar.get_width() / 2,
                bar.get_height() + 1,
                f"{round(val, 2)}%",
                ha="center",
                va="bottom",
            )
    else:
        ax.text(0.5, 0.5, "No possession data", ha="center", va="center", transform=ax.transAxes)
        ax.set_xticks([])
        ax.set_yticks([])

    ax.set_ylim(0, 100)
    ax.set_ylabel("Average estimated possession %")
    ax.set_title("Average possession across all games")

    path = out_dir / f"possession_barchart{suffix}.png"
    fig.savefig(path, dpi=160, bbox_inches="tight")
    plt.close(fig)
    return path


def build_html_report(
    title: str,
    summary_text: str,
    output_dir: Path,
    overall_results: List[dict],
    per_game_results: List[dict],
    team_total_rows: List[dict],
    team_avg_rows: List[dict],
    per_game_team_rows: List[dict],
    player_total_rows: List[dict],
    player_avg_rows: List[dict],
    per_game_player_rows: List[dict],
    visuals: List[Path],
    exclude_cycles_after_restart: int,
    report_file_name: str,
) -> Path:
    def img_src(path: Path) -> str:
        return "data:image/png;base64," + base64.b64encode(path.read_bytes()).decode("ascii")

    def table(rows: List[dict]) -> str:
        if not rows:
            return "<p>No data.</p>"

        headers = list(rows[0].keys())
        thead = "".join(f"<th>{html.escape(str(h))}</th>" for h in headers)
        body = []
        for row in rows:
            body.append("<tr>" + "".join(f"<td>{html.escape(str(row.get(h, '')))}</td>" for h in headers) + "</tr>")
        return f"<div class='table-wrap'><table><thead><tr>{thead}</tr></thead><tbody>{''.join(body)}</tbody></table></div>"

    # Embed images directly so the report is portable as a single HTML file.
    visuals_html = "".join(
        f"<section class='card'><h3>{html.escape(p.stem.replace('_', ' ').title())}</h3><img src='{img_src(p)}' alt='{html.escape(p.name)}'></section>"
        for p in visuals
    )

    report = f"""<!doctype html>
<html>
<head>
<meta charset='utf-8'>
<title>{html.escape(title)}</title>
<style>
body {{ font-family: Arial, sans-serif; margin: 24px; line-height: 1.45; color:#111; background:#fafafa; }}
h1, h2, h3 {{ margin: 0 0 12px 0; }}
.page {{ max-width: 1500px; margin: 0 auto; }}
.card {{ border:1px solid #ddd; border-radius:12px; padding:16px; margin-bottom:16px; background:#fff; box-sizing:border-box; }}
.visual-grid {{ display:grid; grid-template-columns: repeat(auto-fit, minmax(320px,1fr)); gap:16px; }}
img {{ max-width:100%; height:auto; border-radius:8px; display:block; }}
.table-wrap {{ width:100%; overflow-x:auto; }}
table {{ border-collapse: collapse; width:100%; font-size:14px; min-width:760px; }}
th, td {{ border:1px solid #ddd; padding:6px 8px; text-align:left; white-space:nowrap; }}
th {{ background:#f5f5f5; position:sticky; top:0; }}
.small {{ color:#555; }}
</style>
</head>
<body>
<div class='page'>
<h1>{html.escape(title)}</h1>
<p class='small'>{html.escape(summary_text)}</p>

<section class='card'>
<h2>Overall Results</h2>
{table(overall_results)}
</section>

<section class='card'>
<h2>Per Game Results</h2>
{table(per_game_results)}
</section>

<section class='card'>
<h2>Team totals (all games)</h2>
{table(team_total_rows)}
</section>

<section class='card'>
<h2>Team averages per game</h2>
{table(team_avg_rows)}
</section>

<section class='card'>
<h2>Per-game team stats</h2>
{table(per_game_team_rows)}
</section>

<section class='card'>
<h2>Player totals (all games)</h2>
{table(player_total_rows)}
</section>

<section class='card'>
<h2>Player averages per game</h2>
{table(player_avg_rows)}
</section>

<section class='card'>
<h2>Per-game player stats</h2>
{table(per_game_player_rows)}
</section>

<h2>Average visuals across all games</h2>
<div class='visual-grid'>{visuals_html}</div>

<section class='card'>
<p class='small'>
Notes: possession, passes, goals by agent, and assists are estimates inferred from the .rcg state data.
Tactical heatmaps and average formations use play_on only and exclude the first {exclude_cycles_after_restart} cycles after each restart.
Distance travelled is summed across consecutive play_on frames, and cumulative stamina use sums all stamina drops over time.
Match orientation is normalized so the reference team is always on the right side.
</p>
</section>
</div>
</body>
</html>"""

    path = output_dir / report_file_name
    path.write_text(report, encoding="utf-8")
    return path


def build_combined_analyzer(
    rcg_files: List[Path],
    exclude_cycles_after_restart: int,
    reference_team: str,
) -> Tuple[Analyzer, List[dict]]:
    all_frames: List[Frame] = []
    all_playmode_changes: List[Tuple[int, str]] = []
    combined_score_by_cycle: Dict[int, Tuple[int, int]] = {}
    normalized_info: List[dict] = []

    team_names: Optional[Dict[str, str]] = None
    cycle_offset = 0
    score_offset_l = 0
    score_offset_r = 0

    for rcg_path in rcg_files:
        parser = RCGParser(rcg_path).parse()
        frames, names, score_by_cycle, playmode_changes, flipped = normalize_match_to_reference(parser, reference_team)

        if team_names is None:
            team_names = dict(names)
        else:
            if names.get("l") != team_names.get("l") or names.get("r") != team_names.get("r"):
                raise ValueError(f"Normalized team mismatch in {rcg_path.name}. Expected {team_names}, got {names}.")

        match_max_cycle = 0

        for fr in frames:
            new_cycle = fr.cycle + cycle_offset
            all_frames.append(
                Frame(
                    cycle=new_cycle,
                    play_mode=fr.play_mode,
                    ball_x=fr.ball_x,
                    ball_y=fr.ball_y,
                    ball_vx=fr.ball_vx,
                    ball_vy=fr.ball_vy,
                    players=fr.players,
                )
            )
            match_max_cycle = max(match_max_cycle, fr.cycle)

        for cyc, mode in playmode_changes:
            all_playmode_changes.append((cyc + cycle_offset, mode))

        final_l, final_r = (0, 0)
        if score_by_cycle:
            final_l, final_r = score_by_cycle[max(score_by_cycle)]

        combined_score_by_cycle[cycle_offset + match_max_cycle] = (score_offset_l + final_l, score_offset_r + final_r)

        normalized_info.append(
            {
                "source_file": rcg_path.name,
                "normalized_reference_team_to_right": True,
                "file_was_flipped": flipped,
                "normalized_team_names": names,
                "final_score_after_normalization": {"left": final_l, "right": final_r},
            }
        )

        score_offset_l += final_l
        score_offset_r += final_r

        # Add a cycle gap between matches to prevent events from separate files being linked together.
        cycle_offset += match_max_cycle + 1000

    if team_names is None:
        raise ValueError("No matches found.")

    return (
        Analyzer(
            frames=all_frames,
            team_names=team_names,
            score_by_cycle=combined_score_by_cycle,
            playmode_changes=all_playmode_changes,
            exclude_cycles_after_restart=exclude_cycles_after_restart,
        ),
        normalized_info,
    )


def process_match(
    rcg_path: Path,
    exclude_cycles_after_restart: int,
    reference_team: str,
) -> Tuple[List[dict], List[dict], dict]:
    parser = RCGParser(rcg_path).parse()
    frames, team_names, score_by_cycle, playmode_changes, flipped = normalize_match_to_reference(parser, reference_team)

    analyzer = Analyzer(
        frames,
        team_names,
        score_by_cycle,
        playmode_changes,
        exclude_cycles_after_restart=exclude_cycles_after_restart,
    )

    team_rows = analyzer.team_ball_cycles()
    player_rows = analyzer.player_rows()

    left_score, right_score = analyzer.final_score()

    summary = {
        "source_file": rcg_path.name,
        "file_was_flipped": flipped,
        "normalized_team_names": analyzer.team_names,
        "final_score": {"left": left_score, "right": right_score},
        "exclude_cycles_after_restart": exclude_cycles_after_restart,
    }

    return team_rows, player_rows, summary


def make_average_visuals(
    combined_analyzer: Analyzer,
    output_dir: Path,
    team_avg_rows: List[dict],
    game_count: int,
) -> Tuple[List[Path], str]:
    folder_name = f"visuals_average_{game_count}_games"
    vis_dir = output_dir / folder_name
    vis_dir.mkdir(parents=True, exist_ok=True)
    suffix = f"_average_{game_count}_games"

    visuals = [
        make_possession_barchart_from_rows(team_avg_rows, vis_dir, suffix=suffix),
        make_team_zone_bars_from_rows(team_avg_rows, vis_dir, suffix=suffix),
        make_ball_heatmap(combined_analyzer, vis_dir, suffix=suffix),
        make_team_heatmap(combined_analyzer, vis_dir, "l", suffix=suffix),
        make_team_heatmap(combined_analyzer, vis_dir, "r", suffix=suffix),
        make_average_formation(combined_analyzer, vis_dir, "l", suffix=suffix),
        make_average_formation(combined_analyzer, vis_dir, "r", suffix=suffix),
    ]
    visuals += make_player_heatmaps(combined_analyzer, vis_dir, "l", suffix=suffix)
    visuals += make_player_heatmaps(combined_analyzer, vis_dir, "r", suffix=suffix)
    return visuals, folder_name


def build_match_result_rows(match_summaries: List[dict], reference_team: str) -> Tuple[List[dict], List[dict]]:
    per_game_rows: List[dict] = []
    summaries: Dict[str, dict] = {}

    for game_number, match in enumerate(match_summaries, start=1):
        left = match["normalized_team_names"]["l"]
        right = match["normalized_team_names"]["r"]
        ls = match["final_score"]["left"]
        rs = match["final_score"]["right"]

        for team in (left, right):
            if team not in summaries:
                summaries[team] = {
                    "team": team,
                    "games": 0,
                    "wins": 0,
                    "draws": 0,
                    "losses": 0,
                    "goals_scored": 0,
                    "goals_conceded": 0,
                    "goal_difference": 0,
                }

        if ls > rs:
            left_result = "W"
            right_result = "L"
        elif ls < rs:
            left_result = "L"
            right_result = "W"
        else:
            left_result = "D"
            right_result = "D"

        summaries[left]["games"] += 1
        summaries[left]["goals_scored"] += ls
        summaries[left]["goals_conceded"] += rs
        summaries[left]["goal_difference"] += ls - rs
        if left_result == "W":
            summaries[left]["wins"] += 1
        elif left_result == "L":
            summaries[left]["losses"] += 1
        else:
            summaries[left]["draws"] += 1

        summaries[right]["games"] += 1
        summaries[right]["goals_scored"] += rs
        summaries[right]["goals_conceded"] += ls
        summaries[right]["goal_difference"] += rs - ls
        if right_result == "W":
            summaries[right]["wins"] += 1
        elif right_result == "L":
            summaries[right]["losses"] += 1
        else:
            summaries[right]["draws"] += 1

        if right == reference_team:
            ref_score, opp_score = rs, ls
            opponent = left
            result = right_result
        else:
            ref_score, opp_score = ls, rs
            opponent = right
            result = left_result

        per_game_rows.append(
            {
                "game": game_number,
                "opponent": opponent,
                "scoreline": f"{reference_team} {ref_score}-{opp_score} {opponent}",
                "result": result,
            }
        )

    overall_rows = sorted(summaries.values(), key=lambda r: (r["team"] != reference_team, r["team"]))
    return per_game_rows, overall_rows


def apply_readability_fixes(
    all_team_rows: List[dict],
    all_player_rows: List[dict],
    team_total_rows: List[dict],
    team_avg_rows: List[dict],
    player_total_rows: List[dict],
    player_avg_rows: List[dict],
) -> Tuple[List[dict], List[dict], List[dict], List[dict], List[dict], List[dict]]:
    # Recalculate percentage fields after aggregation to avoid averaging derived totals incorrectly.
    team_pos = defaultdict(list)
    for row in all_team_rows:
        team_pos[row["team"]].append(row["estimated_possession_pct"])

    for row in team_avg_rows:
        vals = team_pos.get(row["team"], [])
        row["estimated_possession_pct"] = round(sum(vals) / len(vals), 2) if vals else 0.0

    for row in team_total_rows:
        row["estimated_possession_pct"] = ""

    for row in player_total_rows:
        att = row.get("pass_attempts_est", 0)
        comp = row.get("passes_completed_est", 0)
        row["pass_completion_pct_est"] = round((100.0 * comp / att), 2) if att else 0.0

    for row in player_avg_rows:
        att = row.get("pass_attempts_est", 0)
        comp = row.get("passes_completed_est", 0)
        row["pass_completion_pct_est"] = round((100.0 * comp / att), 2) if att else 0.0

    team_order = [
        "game",
        "team",
        "goals_scored",
        "goals_conceded",
        "goal_difference",
        "estimated_possession_pct",
        "play_on_cycles",
        "ball_cycles_in_own_half",
        "ball_cycles_in_opponent_half",
        "ball_cycles_in_middle_third",
        "ball_cycles_in_own_final_third",
        "ball_cycles_in_opponent_final_third",
        "ball_cycles_in_own_box",
        "ball_cycles_in_opponent_box",
    ]

    player_order = [
        "game",
        "team",
        "player",
        "goals",
        "estimated_assists",
        "pass_attempts_est",
        "passes_completed_est",
        "pass_completion_pct_est",
        "total_stamina_used",
        "distance_travelled_play_on",
    ]

    totals_team_order = [
        "team",
        "goals_scored",
        "goals_conceded",
        "goal_difference",
        "estimated_possession_pct",
        "play_on_cycles",
        "ball_cycles_in_own_half",
        "ball_cycles_in_opponent_half",
        "ball_cycles_in_middle_third",
        "ball_cycles_in_own_final_third",
        "ball_cycles_in_opponent_final_third",
        "ball_cycles_in_own_box",
        "ball_cycles_in_opponent_box",
    ]

    totals_player_order = [
        "team",
        "player",
        "goals",
        "estimated_assists",
        "pass_attempts_est",
        "passes_completed_est",
        "pass_completion_pct_est",
        "total_stamina_used",
        "distance_travelled_play_on",
    ]

    all_team_rows = reorder_rows(round_rows(all_team_rows), team_order)
    all_player_rows = reorder_rows(round_rows(all_player_rows), player_order)
    team_total_rows = reorder_rows(round_rows(team_total_rows), totals_team_order)
    team_avg_rows = reorder_rows(round_rows(team_avg_rows), totals_team_order)
    player_total_rows = reorder_rows(round_rows(player_total_rows), totals_player_order)
    player_avg_rows = reorder_rows(round_rows(player_avg_rows), totals_player_order)

    return all_team_rows, all_player_rows, team_total_rows, team_avg_rows, player_total_rows, player_avg_rows


def main() -> None:
    ap = argparse.ArgumentParser(description="Batch RoboCup 2D analyzer with readable match and player reports")
    ap.add_argument("input_path", type=Path, help="Folder containing .rcg files, or a single .rcg file")
    ap.add_argument(
        "--reference-team",
        type=str,
        default="Ryan_FC",
        help="Team to normalize onto the right side for every match",
    )
    ap.add_argument("--exclude-cycles-after-restart", type=int, default=15)
    ap.add_argument(
        "--output-dir",
        type=Path,
        default=None,
        help="Defaults to the same folder as the input files",
    )
    args = ap.parse_args()

    input_path = args.input_path.resolve()

    if input_path.is_file():
        rcg_files = [input_path]
        default_output_dir = input_path.parent
    else:
        rcg_files = sorted(input_path.glob("*.rcg"))
        default_output_dir = input_path

    if not rcg_files:
        raise FileNotFoundError(f"No .rcg files found in: {input_path}")

    output_dir = args.output_dir.resolve() if args.output_dir else default_output_dir
    output_dir.mkdir(parents=True, exist_ok=True)

    all_team_rows: List[dict] = []
    all_player_rows: List[dict] = []
    match_summaries: List[dict] = []

    for game_number, rcg_path in enumerate(rcg_files, start=1):
        team_rows, player_rows, summary = process_match(
            rcg_path=rcg_path,
            exclude_cycles_after_restart=args.exclude_cycles_after_restart,
            reference_team=args.reference_team,
        )

        for row in team_rows:
            row["game"] = game_number

        for row in player_rows:
            row["game"] = game_number

        all_team_rows.extend(team_rows)
        all_player_rows.extend(player_rows)
        match_summaries.append(summary)

    game_count = len(match_summaries)

    team_total_rows, team_avg_rows = aggregate_rows(
        rows=all_team_rows,
        key_fields=["team"],
        average_divisor=game_count,
    )

    player_total_rows, player_avg_rows = aggregate_rows(
        rows=all_player_rows,
        key_fields=["team", "player"],
        average_divisor=game_count,
    )

    per_game_results, overall_results = build_match_result_rows(match_summaries, args.reference_team)

    (
        all_team_rows,
        all_player_rows,
        team_total_rows,
        team_avg_rows,
        player_total_rows,
        player_avg_rows,
    ) = apply_readability_fixes(
        all_team_rows,
        all_player_rows,
        team_total_rows,
        team_avg_rows,
        player_total_rows,
        player_avg_rows,
    )

    per_game_results = reorder_rows(
        round_rows(per_game_results),
        ["game", "opponent", "scoreline", "result"],
    )
    overall_results = reorder_rows(
        round_rows(overall_results),
        ["team", "games", "wins", "draws", "losses", "goals_scored", "goals_conceded", "goal_difference"],
    )

    combined_analyzer, normalization_info = build_combined_analyzer(
        rcg_files=rcg_files,
        exclude_cycles_after_restart=args.exclude_cycles_after_restart,
        reference_team=args.reference_team,
    )

    visuals, visuals_folder_name = make_average_visuals(combined_analyzer, output_dir, team_avg_rows, game_count)

    left_team = combined_analyzer.team_names["l"]
    right_team = combined_analyzer.team_names["r"]

    summary_text = (
        f"Processed {game_count} game(s). "
        f"Matches were normalized so {args.reference_team} is always on the right side. "
        f"Left team after normalization: {left_team}. Right team after normalization: {right_team}. "
        f"The report includes overall results for both teams, per-game results, team stats, player stats, and average visuals."
    )

    report_file_name = f"report_average_{game_count}_games.html"
    summary_json_name = f"overall_{game_count}_game_summary.json"
    zip_file_name = f"average_{game_count}_games_analysis.zip"

    report_path = build_html_report(
        title=f"Analysis across {game_count} games",
        summary_text=summary_text,
        output_dir=output_dir,
        overall_results=overall_results,
        per_game_results=per_game_results,
        team_total_rows=team_total_rows,
        team_avg_rows=team_avg_rows,
        per_game_team_rows=all_team_rows,
        player_total_rows=player_total_rows,
        player_avg_rows=player_avg_rows,
        per_game_player_rows=all_player_rows,
        visuals=visuals,
        exclude_cycles_after_restart=args.exclude_cycles_after_restart,
        report_file_name=report_file_name,
    )

    overall_summary = {
        "games_processed": game_count,
        "reference_team": args.reference_team,
        "files_processed": [s["source_file"] for s in match_summaries],
        "exclude_cycles_after_restart": args.exclude_cycles_after_restart,
        "normalization_info": normalization_info,
        "overall_results": overall_results,
        "per_game_results": per_game_results,
        "team_totals": team_total_rows,
        "team_averages_per_game": team_avg_rows,
        "per_game_team_stats": all_team_rows,
        "player_totals": player_total_rows,
        "player_averages_per_game": player_avg_rows,
        "per_game_player_stats": all_player_rows,
        "report_file": report_path.name,
        "visuals_folder": visuals_folder_name,
    }

    save_csv(output_dir / "per_game_team_stats.csv", all_team_rows)
    save_csv(output_dir / "per_game_player_stats.csv", all_player_rows)
    save_json(output_dir / "per_game_match_summaries.json", match_summaries)

    save_csv(output_dir / "team_totals_all_games.csv", team_total_rows)
    save_csv(output_dir / "team_averages_per_game.csv", team_avg_rows)
    save_csv(output_dir / "player_totals_all_games.csv", player_total_rows)
    save_csv(output_dir / "player_averages_per_game.csv", player_avg_rows)
    save_csv(output_dir / "per_game_results.csv", per_game_results)
    save_csv(output_dir / "overall_results.csv", overall_results)
    save_json(output_dir / summary_json_name, overall_summary)

    # Package generated reports, tables, and visuals for easy sharing/review.
    zip_path = output_dir / zip_file_name
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as zf:
        for path in output_dir.rglob("*"):
            if path.is_file() and path.name != zip_path.name:
                zf.write(path, arcname=path.relative_to(output_dir))

    print(f"Processed {game_count} game(s).")
    print(f"Outputs written to: {output_dir}")
    print(f"Report: {report_path}")
    print(f"Zip: {zip_path}")


if __name__ == "__main__":
    main()