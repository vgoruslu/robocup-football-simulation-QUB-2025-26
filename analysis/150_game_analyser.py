#!/usr/bin/env python3
# Match-level JSON analyser for RoboCup .rcg logs.
# This version is designed around Ryan_FC vs HELIOS-style fixtures, with reusable parsing and metric logic for Helios v Helios.
from __future__ import annotations

import argparse
import json
import math
import re
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional, Tuple

PLAY_ON = "play_on"

# Zone boundaries used for territory and box-presence metrics.
BOX_X = 36.0
BOX_Y = 20.16
THIRD_LINE = 17.5

RYAN_TEAM_NAME = "Ryan_FC"
HELIOS_TEAM_NAME = "HELIOS_base"

BALL_RE = re.compile(r"\(\(b\)\s+([-\d.eE]+)\s+([-\d.eE]+)\s+([-\d.eE]+)\s+([-\d.eE]+)\)")

# Extract only the player state fields required by the analysis.
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


def save_json(path: Path, obj) -> None:
    path.write_text(json.dumps(obj, indent=2), encoding="utf-8")


def normalize_team_name(name: str) -> str:
    return name.strip().lower()


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
        players = {}

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

        # Skip the immediate post-restart window when building tactical metrics.
        self.tactical_frames = self._filter_tactical_frames()

        self.touch_events = self._build_touch_events()
        self.goals, self.assists = self._estimate_goals_and_assists()
        self.pass_attempts, self.passes_completed = self._estimate_passes()
        self.stamina_usage = self._estimate_stamina_usage()
        self.distance_travelled = self._estimate_distance_travelled()

    def _filter_tactical_frames(self) -> List[Frame]:
        out = []
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
        # Approximate possession from the closest player to the ball.
        best = None
        for (side, unum), p in fr.players.items():
            d = math.hypot(p.x - fr.ball_x, p.y - fr.ball_y)
            if best is None or d < best[0]:
                best = (d, side, unum)
        if best and best[0] <= max_dist:
            return best[1], best[2]
        return None

    def _build_touch_events(self) -> List[TouchEvent]:
        # Record a touch whenever the estimated ball owner changes.
        events = []
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
        # Attribute goals and assists from recent same-team touches before each goal event.
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
        # Count touch-to-touch transitions as pass attempts within the configured cycle window.
        attempts = Counter()
        completed = Counter()
        evs = self.touch_events
        for a, b in zip(evs, evs[1:]):
            gap = b.cycle - a.cycle
            if gap <= 0 or gap > 80:
                continue
            attempts[(a.side, a.unum)] += 1
            if a.side == b.side and a.unum != b.unum:
                completed[(a.side, a.unum)] += 1
        return attempts, completed

    def _estimate_stamina_usage(self) -> Dict[Tuple[str, int], float]:
        # Sum stamina drops only, since stamina can recover during play.
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
        # Sum movement between consecutive play_on frames.
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
        return {s: round(100.0 * counts[s] / total, 2) for s in ("l", "r")}

    def team_ball_cycles(self) -> List[dict]:
        rows = []
        score = self.final_score()
        possession = self.possession_pct()
        play_on_cycles = len(self.play_on_frames)

        for side in ("l", "r"):
            own_half = opp_half = own_final = opp_final = own_box = opp_box = middle = 0
            for fr in self.play_on_frames:
                x, y = fr.ball_x, fr.ball_y

                # Convert absolute pitch coordinates into team-relative territory.
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
                    "side": side,
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
        rows = []
        for side, unum in keys:
            att = self.pass_attempts.get((side, unum), 0)
            comp = self.passes_completed.get((side, unum), 0)
            pct = round(100.0 * comp / att, 2) if att else 0.0
            rows.append(
                {
                    "team": self.team_names[side],
                    "side": side,
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


def resolve_match_sides(team_names: Dict[str, str]) -> Tuple[str, str]:
    # Resolve team orientation from the file so output stays Ryan-first regardless of side.
    left_name = team_names.get("l", "")
    right_name = team_names.get("r", "")

    left_norm = normalize_team_name(left_name)
    right_norm = normalize_team_name(right_name)
    ryan_norm = normalize_team_name(RYAN_TEAM_NAME)
    helios_norm = normalize_team_name(HELIOS_TEAM_NAME)

    if left_norm == ryan_norm and right_norm == helios_norm:
        return "l", "r"

    if left_norm == helios_norm and right_norm == ryan_norm:
        return "r", "l"

    raise ValueError(
        f"Expected teams to be '{RYAN_TEAM_NAME}' vs '{HELIOS_TEAM_NAME}', "
        f"but found: left='{left_name}', right='{right_name}'"
    )


def reorder_team_stats(team_stats: List[dict], ryan_side: str, helios_side: str) -> List[dict]:
    by_side = {row["side"]: row for row in team_stats}
    return [by_side[ryan_side], by_side[helios_side]]


def reorder_player_stats(player_stats: List[dict], ryan_side: str, helios_side: str) -> List[dict]:
    ryan_players = sorted(
        [row for row in player_stats if row["side"] == ryan_side],
        key=lambda x: x["player"],
    )
    helios_players = sorted(
        [row for row in player_stats if row["side"] == helios_side],
        key=lambda x: x["player"],
    )
    return ryan_players + helios_players


def process_file(rcg_path: Path, output_dir: Path, exclude_cycles_after_restart: int) -> None:
    parser = RCGParser(rcg_path).parse()
    ryan_side, helios_side = resolve_match_sides(parser.team_names)

    analyzer = Analyzer(
        parser.frames,
        parser.team_names,
        parser.score_by_cycle,
        parser.playmode_changes,
        exclude_cycles_after_restart=exclude_cycles_after_restart,
    )

    left_score, right_score = analyzer.final_score()
    score_by_side = {"l": left_score, "r": right_score}
    possession_by_side = analyzer.possession_pct()

    raw_team_stats = analyzer.team_ball_cycles()
    raw_player_stats = analyzer.player_rows()

    ordered_team_stats = reorder_team_stats(raw_team_stats, ryan_side, helios_side)
    ordered_player_stats = reorder_player_stats(raw_player_stats, ryan_side, helios_side)

    output = {
        "game_id": rcg_path.stem,
        "source_file": str(rcg_path),
        "teams": {
            "ryan": parser.team_names[ryan_side],
            "helios": parser.team_names[helios_side],
            "left": parser.team_names.get("l", ""),
            "right": parser.team_names.get("r", ""),
        },
        "sides": {
            "ryan": ryan_side,
            "helios": helios_side,
            "left_team": parser.team_names.get("l", ""),
            "right_team": parser.team_names.get("r", ""),
        },
        "final_score": {
            "ryan": score_by_side[ryan_side],
            "helios": score_by_side[helios_side],
            "left": left_score,
            "right": right_score,
        },
        "estimated_possession_pct": {
            "ryan": possession_by_side[ryan_side],
            "helios": possession_by_side[helios_side],
            "left": possession_by_side["l"],
            "right": possession_by_side["r"],
        },
        "team_stats": ordered_team_stats,
        "player_stats": ordered_player_stats,
        "team_stats_by_team": {
            "ryan": ordered_team_stats[0],
            "helios": ordered_team_stats[1],
        },
        "player_stats_by_team": {
            "ryan": [row for row in ordered_player_stats if row["side"] == ryan_side],
            "helios": [row for row in ordered_player_stats if row["side"] == helios_side],
        },
        "exclude_cycles_after_restart": exclude_cycles_after_restart,
        "total_frames": len(analyzer.frames),
        "play_on_frames": len(analyzer.play_on_frames),
    }

    out_file = output_dir / f"{rcg_path.stem}.json"
    save_json(out_file, output)
    print(f"Saved {out_file}")


def main() -> None:
    ap = argparse.ArgumentParser(description="RoboCup Ryan_FC vs HELIOS-base JSON analyzer")
    ap.add_argument(
        "--input-dir",
        type=Path,
        default=Path("/mnt/c/Users/User/RyanFC Results"),
        help="Folder containing .rcg files",
    )
    ap.add_argument(
        "--output-dir",
        type=Path,
        default=Path("/mnt/c/Users/User/RyanFC Results/JsonRyanResults"),
        help="Folder to save JSON output",
    )
    ap.add_argument("--exclude-cycles-after-restart", type=int, default=15)
    args = ap.parse_args()

    args.output_dir.mkdir(parents=True, exist_ok=True)

    rcg_files = sorted(args.input_dir.glob("*.rcg"))
    if not rcg_files:
        print(f"No .rcg files found in {args.input_dir}")
        return

    for rcg_file in rcg_files:
        try:
            process_file(
                rcg_path=rcg_file,
                output_dir=args.output_dir,
                exclude_cycles_after_restart=args.exclude_cycles_after_restart,
            )
        except Exception as e:
            print(f"Failed on {rcg_file.name}: {e}")


if __name__ == "__main__":
    main()