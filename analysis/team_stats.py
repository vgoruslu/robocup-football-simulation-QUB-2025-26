import re
import sys
import math
from collections import defaultdict

import matplotlib.pyplot as plt

if len(sys.argv) != 2:
    print("Usage: python3 team_stats.py match.rcg")
    sys.exit()

rcg_file = sys.argv[1]

left_team = "Left"
right_team = "Right"
left_score = 0
right_score = 0

current_play_mode = ""
seen_cycles = set()

possession = defaultdict(int)
third_counts = defaultdict(int)
half_counts = defaultdict(int)

playon_possession = defaultdict(int)
playon_third_counts = defaultdict(int)
playon_half_counts = defaultdict(int)
playon_ball_positions = []


def get_third(ball_x):
    if ball_x < -17.5:
        return f"{left_team} defensive third"
    elif ball_x > 17.5:
        return f"{right_team} defensive third"
    else:
        return "Middle third"


def get_half(ball_x):
    if ball_x < 0:
        return f"{left_team} half"
    else:
        return f"{right_team} half"


def draw_pitch():
    plt.xlim(-52.5, 52.5)
    plt.ylim(-34, 34)

    plt.plot([-52.5, 52.5], [-34, -34])
    plt.plot([-52.5, 52.5], [34, 34])
    plt.plot([-52.5, -52.5], [-34, 34])
    plt.plot([52.5, 52.5], [-34, 34])
    plt.plot([0, 0], [-34, 34])

    centre_circle = plt.Circle((0, 0), 9.15, fill=False)
    plt.gca().add_patch(centre_circle)

    plt.plot([-52.5, -36], [-20.16, -20.16])
    plt.plot([-36, -36], [-20.16, 20.16])
    plt.plot([-36, -52.5], [20.16, 20.16])

    plt.plot([52.5, 36], [-20.16, -20.16])
    plt.plot([36, 36], [-20.16, 20.16])
    plt.plot([36, 52.5], [20.16, 20.16])


def save_possession_pie(filename, title, poss):
    labels = [left_team, right_team]
    values = [poss["l"], poss["r"]]

    plt.figure(figsize=(6, 6))
    plt.pie(values, labels=labels, autopct="%1.1f%%", startangle=90)
    plt.title(title)
    plt.tight_layout()
    plt.savefig(filename)
    plt.close()


def save_thirds_bar(filename, title, thirds):
    areas = [f"{left_team} defensive third", "Middle third", f"{right_team} defensive third"]
    values = [thirds[a] for a in areas]

    plt.figure(figsize=(9, 5))
    plt.bar(areas, values)
    plt.title(title)
    plt.ylabel("Cycles")
    plt.xticks(rotation=15)
    plt.tight_layout()
    plt.savefig(filename)
    plt.close()


def save_ball_heatmap(filename, title, positions):
    if not positions:
        return

    x = [p[0] for p in positions]
    y = [p[1] for p in positions]

    plt.figure(figsize=(10, 6))
    draw_pitch()
    plt.hist2d(x, y, bins=45, alpha=0.75)
    plt.colorbar(label="Ball frequency")
    plt.title(title)
    plt.tight_layout()
    plt.savefig(filename)
    plt.close()


def print_stats(title, poss, thirds, halves):
    total_possession = poss["l"] + poss["r"]
    total_thirds = sum(thirds.values())
    total_halves = sum(halves.values())

    left_percent = poss["l"] / total_possession * 100 if total_possession > 0 else 0
    right_percent = poss["r"] / total_possession * 100 if total_possession > 0 else 0

    print(title)
    print()
    print("Estimated Possession")
    print("(assigned each cycle to the team with the closest player to the ball)")
    print(f"{left_team}: {left_percent:.1f}% ({poss['l']} cycles)")
    print(f"{right_team}: {right_percent:.1f}% ({poss['r']} cycles)")
    print()

    print("Ball Location by Thirds")
    for area in [f"{left_team} defensive third", "Middle third", f"{right_team} defensive third"]:
        count = thirds[area]
        percent = count / total_thirds * 100 if total_thirds > 0 else 0
        print(f"{area}: {percent:.1f}% ({count} cycles)")

    print()
    print("Ball Location by Halves")
    for area in [f"{left_team} half", f"{right_team} half"]:
        count = halves[area]
        percent = count / total_halves * 100 if total_halves > 0 else 0
        print(f"{area}: {percent:.1f}% ({count} cycles)")
    print()


with open(rcg_file, "r", errors="ignore") as f:
    for line in f:

        if line.startswith("(team"):
            match = re.search(r"\(team\s+\d+\s+(\S+)\s+(\S+)\s+(\d+)\s+(\d+)\)", line)
            if match:
                left_team = match.group(1)
                right_team = match.group(2)
                left_score = int(match.group(3))
                right_score = int(match.group(4))

        elif line.startswith("(playmode"):
            match = re.search(r"\(playmode\s+\d+\s+(\S+)\)", line)
            if match:
                current_play_mode = match.group(1)

        elif line.startswith("(show"):
            cycle_match = re.search(r"\(show\s+(\d+)", line)
            if not cycle_match:
                continue

            cycle = int(cycle_match.group(1))

            if cycle > 6000:
                continue

            if cycle in seen_cycles:
                continue

            seen_cycles.add(cycle)

            ball_match = re.search(r"\(\(b\)\s+(-?\d+\.?\d*)\s+(-?\d+\.?\d*)", line)
            if not ball_match:
                continue

            ball_x = float(ball_match.group(1))
            ball_y = float(ball_match.group(2))

            third = get_third(ball_x)
            half = get_half(ball_x)

            players = re.findall(
                r"\(\(([lr])\s+\d+\).*?\s(-?\d+\.?\d*)\s+(-?\d+\.?\d*)\s",
                line
            )

            closest_side = None
            closest_distance = float("inf")

            # Estimate possession from the nearest player to the ball.
            for side, x, y in players:
                x = float(x)
                y = float(y)

                distance = math.sqrt((x - ball_x) ** 2 + (y - ball_y) ** 2)

                if distance < closest_distance:
                    closest_distance = distance
                    closest_side = side

            third_counts[third] += 1
            half_counts[half] += 1

            if closest_side:
                possession[closest_side] += 1

            # Track play_on cycles separately from stoppages.
            if current_play_mode == "play_on":
                playon_ball_positions.append((ball_x, ball_y))
                playon_third_counts[third] += 1
                playon_half_counts[half] += 1

                if closest_side:
                    playon_possession[closest_side] += 1


total_all_cycles = sum(third_counts.values())
total_playon_cycles = sum(playon_third_counts.values())
total_deadball_cycles = total_all_cycles - total_playon_cycles

playon_percent = total_playon_cycles / total_all_cycles * 100 if total_all_cycles > 0 else 0
deadball_percent = total_deadball_cycles / total_all_cycles * 100 if total_all_cycles > 0 else 0

print("===== RoboCup 2D Match Stats =====")
print()
print("Final Score:")
print(f"{left_team} {left_score} - {right_score} {right_team}")
print()

print("Cycle Summary:")
print(f"Total counted cycles: {total_all_cycles}")
print(f"Play on cycles: {total_playon_cycles} ({playon_percent:.1f}%)")
print(f"Dead-ball / stoppage cycles: {total_deadball_cycles} ({deadball_percent:.1f}%)")
print()

print_stats("===== All Cycles Stats =====", possession, third_counts, half_counts)
print_stats("===== Play On Only Stats =====", playon_possession, playon_third_counts, playon_half_counts)

save_possession_pie(
    "playon_possession_pie.png",
    "Estimated Possession (Play On Only)",
    playon_possession
)

save_thirds_bar(
    "playon_territory_thirds.png",
    "Ball Location by Thirds (Play On Only)",
    playon_third_counts
)

save_ball_heatmap(
    "playon_ball_heatmap_pitch.png",
    "Ball Heatmap (Play On Only)",
    playon_ball_positions
)

print("Play-on visualisations saved:")
print("- playon_possession_pie.png")
print("- playon_territory_thirds.png")
print("- playon_ball_heatmap_pitch.png")