#!/usr/bin/env python3
# Aggregate report builder for per-match RoboCup JSON outputs.
# Consumes match-level exports and produces summary tables, charts, and a self-contained HTML report.
from __future__ import annotations

import base64
import csv
import html
import io
import json
from collections import defaultdict
from pathlib import Path
from statistics import mean

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

OUTPUT_DIR = Path("/mnt/c/Users/User/RyanFC Results/JsonRyanResults")


def save_json(path: Path, obj) -> None:
    path.write_text(json.dumps(obj, indent=2), encoding="utf-8")


def save_csv(path: Path, rows: list[dict]) -> None:
    if not rows:
        path.write_text("", encoding="utf-8")
        return

    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)


def load_game_jsons(folder: Path) -> list[dict]:
    files = sorted(folder.glob("*.json"))

    # Exclude generated aggregate files so only match-level JSON exports are loaded.
    files = [f for f in files if not f.name.startswith("aggregate_")]

    data = []
    for f in files:
        try:
            data.append(json.loads(f.read_text(encoding="utf-8")))
        except Exception as e:
            print(f"Skipping {f.name}: {e}")

    return data


def fig_to_base64(fig) -> str:
    # Embed charts directly in the HTML to keep the report portable.
    buf = io.BytesIO()
    fig.savefig(buf, format="png", dpi=160, bbox_inches="tight")
    plt.close(fig)
    return base64.b64encode(buf.getvalue()).decode("ascii")


def make_bar_chart(labels, values, title, ylabel) -> str:
    fig, ax = plt.subplots(figsize=(7, 4.5))
    bars = ax.bar(labels, values)

    ax.set_title(title)
    ax.set_ylabel(ylabel)

    for bar, val in zip(bars, values):
        ax.text(
            bar.get_x() + bar.get_width() / 2,
            bar.get_height(),
            f"{val:.2f}",
            ha="center",
            va="bottom",
        )

    return fig_to_base64(fig)


def make_line_chart(xs, ys1, ys2, label1, label2, title, ylabel) -> str:
    fig, ax = plt.subplots(figsize=(9, 4.8))

    ax.plot(xs, ys1, label=label1)
    ax.plot(xs, ys2, label=label2)

    ax.set_title(title)
    ax.set_xlabel("Game")
    ax.set_ylabel(ylabel)
    ax.legend()

    return fig_to_base64(fig)


def make_histogram(values, title, xlabel) -> str:
    fig, ax = plt.subplots(figsize=(7, 4.5))

    ax.hist(values, bins=15)
    ax.set_title(title)
    ax.set_xlabel(xlabel)
    ax.set_ylabel("Frequency")

    return fig_to_base64(fig)


def html_table(rows: list[dict], max_rows: int | None = None) -> str:
    if not rows:
        return "<p>No data.</p>"

    display_rows = rows if max_rows is None else rows[:max_rows]
    headers = list(display_rows[0].keys())

    thead = "".join(f"<th>{html.escape(str(h))}</th>" for h in headers)

    body = []
    for row in display_rows:
        body.append(
            "<tr>"
            + "".join(f"<td>{html.escape(str(row.get(h, '')))}</td>" for h in headers)
            + "</tr>"
        )

    note = ""
    if max_rows is not None and len(rows) > max_rows:
        note = f"<p class='small'>Showing first {max_rows} of {len(rows)} rows.</p>"

    return (
        note
        + "<div class='table-wrap'><table><thead><tr>"
        + thead
        + "</tr></thead><tbody>"
        + "".join(body)
        + "</tbody></table></div>"
    )


def safe_mean(values: list[float]) -> float:
    return mean(values) if values else 0.0


def main() -> None:
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

    games = load_game_jsons(OUTPUT_DIR)
    if not games:
        print("No per-game JSON files found.")
        return

    total_games = len(games)

    wins = {"ryan": 0, "helios": 0, "draw": 0}
    goals_ryan = []
    goals_helios = []
    poss_ryan = []
    poss_helios = []
    play_on_frames = []
    goal_diffs = []

    team_metric_lists = defaultdict(list)
    player_metric_lists = defaultdict(lambda: defaultdict(list))
    per_game_summary_rows = []

    team_name_ryan = "Ryan_FC"
    team_name_helios = "HELIOS_base"

    for idx, game in enumerate(games, start=1):
        gl = game["final_score"]["ryan"]
        gr = game["final_score"]["helios"]
        pl = game["estimated_possession_pct"]["ryan"]
        pr = game["estimated_possession_pct"]["helios"]

        goals_ryan.append(gl)
        goals_helios.append(gr)
        poss_ryan.append(pl)
        poss_helios.append(pr)
        play_on_frames.append(game.get("play_on_frames", 0))
        goal_diffs.append(gl - gr)

        team_name_ryan = game.get("teams", {}).get("ryan", team_name_ryan)
        team_name_helios = game.get("teams", {}).get("helios", team_name_helios)

        if gl > gr:
            wins["ryan"] += 1
            result = "ryan_win"
        elif gr > gl:
            wins["helios"] += 1
            result = "helios_win"
        else:
            wins["draw"] += 1
            result = "draw"

        per_game_summary_rows.append(
            {
                "game_index": idx,
                "game_id": game["game_id"],
                "ryan_team": game["teams"]["ryan"],
                "helios_team": game["teams"]["helios"],
                "ryan_score": gl,
                "helios_score": gr,
                "goal_difference_ryan_minus_helios": gl - gr,
                "ryan_possession_pct_est": pl,
                "helios_possession_pct_est": pr,
                "result": result,
                "play_on_frames": game.get("play_on_frames", 0),
            }
        )

        # Collect team metrics across all games for aggregate reporting.
        for team_key in ("ryan", "helios"):
            team_row = game.get("team_stats_by_team", {}).get(team_key)
            if not team_row:
                continue

            for key, value in team_row.items():
                if key in {"team", "side"}:
                    continue
                if isinstance(value, (int, float)):
                    team_metric_lists[(team_key, key)].append(value)

        # Group players by team and uniform number for cross-game comparison.
        for team_key in ("ryan", "helios"):
            player_rows = game.get("player_stats_by_team", {}).get(team_key, [])
            for player_row in player_rows:
                unum = player_row["player"]

                for key, value in player_row.items():
                    if key in {"team", "side", "player"}:
                        continue
                    if isinstance(value, (int, float)):
                        player_metric_lists[(team_key, unum)][key].append(value)

    aggregate_summary = {
        "total_games": total_games,
        "teams": {
            "ryan": team_name_ryan,
            "helios": team_name_helios,
        },
        "win_rates_pct": {
            "ryan": round(wins["ryan"] * 100.0 / total_games, 2),
            "helios": round(wins["helios"] * 100.0 / total_games, 2),
            "draw": round(wins["draw"] * 100.0 / total_games, 2),
        },
        "average_goals": {
            "ryan": round(safe_mean(goals_ryan), 3),
            "helios": round(safe_mean(goals_helios), 3),
        },
        "total_goals": {
            "ryan": sum(goals_ryan),
            "helios": sum(goals_helios),
        },
        "average_goal_difference_ryan_minus_helios": round(safe_mean(goal_diffs), 3),
        "average_possession_pct": {
            "ryan": round(safe_mean(poss_ryan), 3),
            "helios": round(safe_mean(poss_helios), 3),
        },
        "average_play_on_frames": round(safe_mean(play_on_frames), 3),
    }

    team_average_rows = []
    for (team_key, metric), values in sorted(team_metric_lists.items()):
        team_average_rows.append(
            {
                "team_key": team_key,
                "team": team_name_ryan if team_key == "ryan" else team_name_helios,
                "metric": metric,
                "average": round(safe_mean(values), 4),
                "min": round(min(values), 4),
                "max": round(max(values), 4),
            }
        )

    player_average_rows = []
    for (team_key, unum), metrics in sorted(
        player_metric_lists.items(),
        key=lambda item: (item[0][0], item[0][1]),
    ):
        row = {
            "team_key": team_key,
            "team": team_name_ryan if team_key == "ryan" else team_name_helios,
            "player": unum,
        }

        for metric, values in sorted(metrics.items()):
            row[f"{metric}_avg"] = round(safe_mean(values), 4)
            row[f"{metric}_min"] = round(min(values), 4)
            row[f"{metric}_max"] = round(max(values), 4)

        player_average_rows.append(row)

    # Write machine-readable exports alongside the HTML report.
    save_json(OUTPUT_DIR / "aggregate_summary.json", aggregate_summary)
    save_csv(OUTPUT_DIR / "aggregate_per_game_summary.csv", per_game_summary_rows)
    save_csv(OUTPUT_DIR / "aggregate_team_metric_averages.csv", team_average_rows)
    save_csv(OUTPUT_DIR / "aggregate_player_metric_averages.csv", player_average_rows)

    game_indices = list(range(1, total_games + 1))

    win_rate_chart = make_bar_chart(
        [team_name_ryan, team_name_helios, "Draw"],
        [
            aggregate_summary["win_rates_pct"]["ryan"],
            aggregate_summary["win_rates_pct"]["helios"],
            aggregate_summary["win_rates_pct"]["draw"],
        ],
        "Win rate %",
        "Percentage",
    )

    avg_goals_chart = make_bar_chart(
        [team_name_ryan, team_name_helios],
        [
            aggregate_summary["average_goals"]["ryan"],
            aggregate_summary["average_goals"]["helios"],
        ],
        "Average goals per game",
        "Goals",
    )

    avg_poss_chart = make_bar_chart(
        [team_name_ryan, team_name_helios],
        [
            aggregate_summary["average_possession_pct"]["ryan"],
            aggregate_summary["average_possession_pct"]["helios"],
        ],
        "Average estimated possession %",
        "Possession %",
    )

    score_trend_chart = make_line_chart(
        game_indices,
        goals_ryan,
        goals_helios,
        team_name_ryan,
        team_name_helios,
        "Goals by game",
        "Goals",
    )

    possession_trend_chart = make_line_chart(
        game_indices,
        poss_ryan,
        poss_helios,
        team_name_ryan,
        team_name_helios,
        "Estimated possession by game",
        "Possession %",
    )

    goal_diff_hist_chart = make_histogram(
        goal_diffs,
        "Goal difference distribution (Ryan - HELIOS)",
        "Goal difference",
    )

    top_players = sorted(
        player_average_rows,
        key=lambda r: (
            r.get("goals_avg", 0),
            r.get("estimated_assists_avg", 0),
            r.get("passes_completed_est_avg", 0),
        ),
        reverse=True,
    )

    summary_cards = [
        ("Total games", str(aggregate_summary["total_games"])),
        ("Average goals Ryan", str(aggregate_summary["average_goals"]["ryan"])),
        ("Average goals HELIOS", str(aggregate_summary["average_goals"]["helios"])),
        ("Average possession Ryan", f'{aggregate_summary["average_possession_pct"]["ryan"]}%'),
        ("Average possession HELIOS", f'{aggregate_summary["average_possession_pct"]["helios"]}%'),
        ("Ryan win rate", f'{aggregate_summary["win_rates_pct"]["ryan"]}%'),
        ("HELIOS win rate", f'{aggregate_summary["win_rates_pct"]["helios"]}%'),
        ("Draw rate", f'{aggregate_summary["win_rates_pct"]["draw"]}%'),
    ]

    summary_cards_html = "".join(
        f"<div class='stat-card'>"
        f"<div class='stat-label'>{html.escape(label)}</div>"
        f"<div class='stat-value'>{html.escape(value)}</div>"
        f"</div>"
        for label, value in summary_cards
    )

    report_html = f"""<!doctype html>
<html>
<head>
<meta charset="utf-8">
<title>Ryan FC Aggregate Report</title>
<style>
body {{
  font-family: Arial, sans-serif;
  margin: 24px;
  background: #f7f7f7;
  color: #111;
}}
.page {{
  max-width: 1500px;
  margin: 0 auto;
}}
h1, h2, h3 {{
  margin: 0 0 12px 0;
}}
.card {{
  background: white;
  border: 1px solid #ddd;
  border-radius: 12px;
  padding: 16px;
  margin-bottom: 16px;
}}
.stats-grid {{
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(180px, 1fr));
  gap: 12px;
}}
.stat-card {{
  background: #fafafa;
  border: 1px solid #e2e2e2;
  border-radius: 10px;
  padding: 14px;
}}
.stat-label {{
  font-size: 13px;
  color: #555;
  margin-bottom: 6px;
}}
.stat-value {{
  font-size: 24px;
  font-weight: bold;
}}
.chart-grid {{
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(420px, 1fr));
  gap: 16px;
}}
.chart-grid img {{
  width: 100%;
  height: auto;
  border-radius: 8px;
}}
.table-wrap {{
  overflow-x: auto;
}}
table {{
  border-collapse: collapse;
  width: 100%;
  min-width: 900px;
  font-size: 14px;
}}
th, td {{
  border: 1px solid #ddd;
  padding: 6px 8px;
  text-align: left;
  white-space: nowrap;
}}
th {{
  background: #f0f0f0;
}}
.small {{
  color: #666;
  font-size: 13px;
}}
</style>
</head>
<body>
<div class="page">
  <div class="card">
    <h1>Ryan FC Aggregate Report</h1>
    <p class="small">
      Aggregate report across {aggregate_summary["total_games"]} games for
      {html.escape(team_name_ryan)} vs {html.escape(team_name_helios)}.
    </p>
  </div>

  <div class="card">
    <h2>Summary</h2>
    <div class="stats-grid">
      {summary_cards_html}
    </div>
  </div>

  <div class="card">
    <h2>Charts</h2>
    <div class="chart-grid">
      <div><h3>Win rate</h3><img src="data:image/png;base64,{win_rate_chart}" alt="Win rate"></div>
      <div><h3>Average goals</h3><img src="data:image/png;base64,{avg_goals_chart}" alt="Average goals"></div>
      <div><h3>Average possession</h3><img src="data:image/png;base64,{avg_poss_chart}" alt="Average possession"></div>
      <div><h3>Goals by game</h3><img src="data:image/png;base64,{score_trend_chart}" alt="Goals by game"></div>
      <div><h3>Possession by game</h3><img src="data:image/png;base64,{possession_trend_chart}" alt="Possession by game"></div>
      <div><h3>Goal difference distribution</h3><img src="data:image/png;base64,{goal_diff_hist_chart}" alt="Goal difference distribution"></div>
    </div>
  </div>

  <div class="card">
    <h2>Aggregate summary JSON</h2>
    <pre>{html.escape(json.dumps(aggregate_summary, indent=2))}</pre>
  </div>

  <div class="card">
    <h2>Per-game summary</h2>
    {html_table(per_game_summary_rows)}
  </div>

  <div class="card">
    <h2>Team metric averages</h2>
    {html_table(team_average_rows)}
  </div>

  <div class="card">
    <h2>Top players by aggregate attacking output</h2>
    {html_table(top_players, max_rows=20)}
  </div>

  <div class="card">
    <h2>All player metric averages</h2>
    {html_table(player_average_rows)}
  </div>
</div>
</body>
</html>
"""

    html_path = OUTPUT_DIR / "aggregate_report_self_contained.html"
    html_path.write_text(report_html, encoding="utf-8")

    print("Saved:")
    print(OUTPUT_DIR / "aggregate_summary.json")
    print(OUTPUT_DIR / "aggregate_per_game_summary.csv")
    print(OUTPUT_DIR / "aggregate_team_metric_averages.csv")
    print(OUTPUT_DIR / "aggregate_player_metric_averages.csv")
    print(html_path)


if __name__ == "__main__":
    main()