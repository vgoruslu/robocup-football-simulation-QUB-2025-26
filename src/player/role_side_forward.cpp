// -*-c++-*-
/*
 *Copyright:
 *Copyright (C) Hidehisa AKIYAMA
 *
 *This code is free software; you can redistribute it and/or modify
 *it under the terms of the GNU General Public License as published by
 *the Free Software Foundation; either version 3, or (at your option)
 *any later version.
 *
 *This code is distributed in the hope that it will be useful,
 *but WITHOUT ANY WARRANTY; without even the implied warranty of
 *MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *See the GNU General Public License for more details.
 *
 *You should have received a copy of the GNU General Public License
 *along with this code; see the file COPYING. If not, write to
 *the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 *EndCopyright:
 */

/////////////////////////////////////////////////////////////////////

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "role_side_forward.h"
#include "bhv_basic_move.h"
#include "planner/bhv_planned_action.h"
#include "basic_actions/body_hold_ball.h"
#include "basic_actions/neck_scan_field.h"
#include "basic_actions/body_smart_kick.h"
#include "basic_actions/body_dribble2008.h"
#include "strategy.h"

#include <rcsc/player/world_model.h>
#include <rcsc/player/player_agent.h>
#include <rcsc/player/debug_client.h>
#include <rcsc/common/server_param.h>
#include <algorithm>
#include <cmath>

using namespace rcsc;

const std::string RoleSideForward::NAME("SideForward");

namespace
{
    rcss::RegHolder role = SoccerRole::creators().autoReg(
        &RoleSideForward::create,
        RoleSideForward::NAME);
}

bool RoleSideForward::execute(PlayerAgent *agent)
{
    bool kickable = agent->world().self().isKickable();

    if (agent->world().kickableTeammate()
        && !agent->world().teammatesFromBall().empty()
        && agent->world().teammatesFromBall().front()->distFromBall()
               < agent->world().ball().distFromSelf())
    {
        kickable = false;
    }

    if (kickable)
    {
        doKick(agent);
    }
    else
    {
        doMove(agent);
    }

    return true;
}

void RoleSideForward::doKick(PlayerAgent *agent)
{
    const WorldModel &wm = agent->world();
    const Vector2D ball = wm.ball().pos();

    // Special handling for wide attacking positions near the final third.
    if (ball.x > 36.0 && std::fabs(ball.y) > 6.0 && std::fabs(ball.y) < 20.0)
    {
        const AbstractPlayerObject *penalty_spot = nullptr;
        const AbstractPlayerObject *near_post = nullptr;
        const AbstractPlayerObject *far_post = nullptr;
        const AbstractPlayerObject *edge_box = nullptr;
        const AbstractPlayerObject *inside_pocket = nullptr;
        const AbstractPlayerObject *recycle = nullptr;
        const AbstractPlayerObject *goalie = nullptr;

        for (auto o = wm.opponentsFromSelf().begin();
             o != wm.opponentsFromSelf().end();
             ++o)
        {
            if (!(*o))
                continue;
            if ((*o)->goalie())
            {
                goalie = *o;
                break;
            }
        }

        for (auto t = wm.teammatesFromBall().begin();
             t != wm.teammatesFromBall().end();
             ++t)
        {
            if (!(*t))
                continue;
            const Vector2D pos = (*t)->pos();

            if (pos.x > 30.0 && std::fabs(pos.y) < 9.0)
            {
                if (!penalty_spot || pos.x > penalty_spot->pos().x)
                {
                    penalty_spot = *t;
                }
            }

            if (pos.x > 36.0 && std::fabs(pos.y) < 11.0)
            {
                if (!near_post || ball.dist(pos) < ball.dist(near_post->pos()))
                {
                    near_post = *t;
                }
            }

            if (pos.x > 32.0 && std::fabs(pos.y) > 5.0 && ball.y * pos.y < 0.0)
            {
                if (!far_post || pos.x > far_post->pos().x)
                {
                    far_post = *t;
                }
            }

            if (pos.x > 24.0 && pos.x < 38.0 && std::fabs(pos.y) < 16.0)
            {
                if (!edge_box || ball.dist(pos) < ball.dist(edge_box->pos()))
                {
                    edge_box = *t;
                }
            }

            if (pos.x > ball.x - 7.0
                && pos.x < ball.x + 7.0
                && std::fabs(pos.y) < std::fabs(ball.y)
                && std::fabs(pos.y) > 1.5)
            {
                if (!inside_pocket
                    || pos.x > inside_pocket->pos().x
                    || std::fabs(pos.y) < std::fabs(inside_pocket->pos().y))
                {
                    inside_pocket = *t;
                }
            }

            if (pos.x > 20.0 && pos.x < ball.x && std::fabs(pos.y) < 20.0)
            {
                if (!recycle || ball.dist(pos) < ball.dist(recycle->pos()))
                {
                    recycle = *t;
                }
            }
        }

        double nearest_opp_to_self = 100.0;
        for (auto o = wm.opponentsFromSelf().begin();
             o != wm.opponentsFromSelf().end();
             ++o)
        {
            if (!(*o))
                continue;
            nearest_opp_to_self = std::min(nearest_opp_to_self, (*o)->distFromSelf());
        }

        auto lane_is_dead = [&](const Vector2D &from, const Vector2D &to) -> bool
        {
            const Vector2D line = to - from;
            const double len2 = line.r2();
            if (len2 < 0.0001)
                return true;

            for (auto o = wm.opponentsFromBall().begin();
                 o != wm.opponentsFromBall().end();
                 ++o)
            {
                if (!(*o))
                    continue;
                const Vector2D op = (*o)->pos();
                const double proj = ((op - from).innerProduct(line)) / len2;
                if (proj < 0.08 || proj > 0.92)
                    continue;
                const Vector2D foot = from + line * proj;
                const double perp = op.dist(foot);
                if (perp < 0.9)
                    return true;
            }
            return false;
        };

        auto lane_block_penalty = [&](const Vector2D &from, const Vector2D &to) -> double
        {
            const Vector2D line = to - from;
            const double len2 = line.r2();
            if (len2 < 0.0001)
                return 10.0;

            double penalty = 0.0;
            for (auto o = wm.opponentsFromBall().begin();
                 o != wm.opponentsFromBall().end();
                 ++o)
            {
                if (!(*o))
                    continue;
                const Vector2D op = (*o)->pos();
                const double proj = ((op - from).innerProduct(line)) / len2;
                if (proj < 0.04 || proj > 0.96)
                    continue;
                const Vector2D foot = from + line * proj;
                const double perp = op.dist(foot);

                if (perp < 1.0)
                    penalty += 8.0;
                else if (perp < 1.8)
                    penalty += 4.3;
                else if (perp < 3.0)
                    penalty += (3.0 - perp) * 1.4;
            }
            return penalty;
        };

        auto nearby_opponents = [&](const Vector2D &pos, const double radius) -> int
        {
            int count = 0;
            for (auto o = wm.opponentsFromBall().begin();
                 o != wm.opponentsFromBall().end();
                 ++o)
            {
                if (!(*o))
                    continue;
                if ((*o)->pos().dist(pos) < radius)
                    ++count;
            }
            return count;
        };

        auto can_attack_more_space = [&](const Vector2D &from,
                                         Vector2D *target_out,
                                         double *score_out) -> bool
        {
            const double side = (from.y > 0.0 ? 1.0 : -1.0);

            Vector2D candidates[5] = {
                Vector2D(from.x + 2.2, from.y - side * 1.2),
                Vector2D(from.x + 3.0, from.y - side * 2.0),
                Vector2D(from.x + 3.8, from.y - side * 2.6),
                Vector2D(from.x + 4.2, from.y - side * 3.0),
                Vector2D(from.x + 2.8, from.y - side * 3.5)};

            double best_score = -1000.0;
            Vector2D best_target;

            for (const Vector2D &cand_raw : candidates)
            {
                Vector2D cand = cand_raw;
                cand.x = std::min(50.8, cand.x);
                cand.y = min_max(-ServerParam::i().pitchHalfWidth() + 1.0,
                                 cand.y,
                                 ServerParam::i().pitchHalfWidth() - 1.0);

                if (cand.x < from.x + 1.7)
                    continue;
                if (std::fabs(cand.y) >= std::fabs(from.y))
                    continue;

                int nearby = 0;
                double nearest_to_target = 100.0;
                for (auto o = wm.opponentsFromBall().begin();
                     o != wm.opponentsFromBall().end();
                     ++o)
                {
                    if (!(*o))
                        continue;
                    const double d = (*o)->pos().dist(cand);
                    nearest_to_target = std::min(nearest_to_target, d);
                    if (d < 2.4)
                        ++nearby;
                }

                if (nearest_to_target < 1.8)
                    continue;

                const Vector2D line = cand - from;
                const double len2 = line.r2();
                if (len2 < 0.001)
                    continue;

                double lane_penalty = 0.0;
                for (auto o = wm.opponentsFromBall().begin();
                     o != wm.opponentsFromBall().end();
                     ++o)
                {
                    if (!(*o))
                        continue;
                    const Vector2D op = (*o)->pos();
                    const double proj = ((op - from).innerProduct(line)) / len2;
                    if (proj < 0.05 || proj > 1.0)
                        continue;
                    const Vector2D foot = from + line * proj;
                    const double perp = op.dist(foot);

                    if (perp < 1.1)
                        lane_penalty += 3.0;
                    else if (perp < 1.8)
                        lane_penalty += 1.5;
                }

                double score = 0.0;
                score += (cand.x - from.x) * 2.1;
                score += (std::fabs(from.y) - std::fabs(cand.y)) * 1.6;
                score += std::min(nearest_to_target, 5.0) * 1.0;
                score -= lane_penalty;
                score -= nearby * 0.8;

                if (score > best_score)
                {
                    best_score = score;
                    best_target = cand;
                }
            }

            if (best_score < 4.2)
                return false;
            if (target_out)
                *target_out = best_target;
            if (score_out)
                *score_out = best_score;
            return true;
        };

        auto eval_target = [&](const AbstractPlayerObject *target,
                               const double base_score,
                               const bool finishing_zone) -> double
        {
            if (!target)
                return -1000.0;
            const Vector2D pos = target->pos();
            if (lane_is_dead(ball, pos))
                return -1000.0;

            double score = base_score;
            score += pos.x * 0.10;
            score -= std::fabs(pos.y) * 0.11;
            score -= ball.dist(pos) * 0.05;
            score -= lane_block_penalty(ball, pos);

            const Vector2D pass_vec = pos - ball;
            if (pass_vec.x < -1.0)
                score -= 2.2;
            if (pass_vec.x < -3.0)
                score -= 1.8;

            int close_markers = 0;
            int loose_markers = 0;
            for (auto o = wm.opponentsFromBall().begin();
                 o != wm.opponentsFromBall().end();
                 ++o)
            {
                if (!(*o))
                    continue;
                const double d = (*o)->pos().dist(pos);
                if (d < 2.5)
                    ++close_markers;
                else if (d < 4.0)
                    ++loose_markers;
            }

            score -= close_markers * 2.4;
            score -= loose_markers * 0.75;

            if (goalie && finishing_zone)
            {
                const double gd = goalie->pos().dist(pos);
                if (gd < 5.0)
                    score -= (5.0 - gd) * 1.0;
            }

            return score;
        };

        int support_count = 0;
        if (inside_pocket)
            ++support_count;
        if (edge_box)
            ++support_count;
        if (recycle)
            ++support_count;
        if (penalty_spot)
            ++support_count;

        double penalty_score = eval_target(penalty_spot, 8.0, true);
        double near_score = eval_target(near_post, 7.2, true);
        double far_score = eval_target(far_post, 7.0, true);
        double edge_score = eval_target(edge_box, 6.4, false);

        const AbstractPlayerObject *best_cutback_target = nullptr;
        double best_cutback_target_score = -1000.0;

        auto consider_cutback = [&](const AbstractPlayerObject *t, const double s)
        {
            if (t && s > best_cutback_target_score)
            {
                best_cutback_target_score = s;
                best_cutback_target = t;
            }
        };

        consider_cutback(penalty_spot, penalty_score);
        consider_cutback(near_post, near_score);
        consider_cutback(far_post, far_score);
        consider_cutback(edge_box, edge_score);

        double cutback_score = -1000.0;
        if (best_cutback_target)
        {
            cutback_score = best_cutback_target_score;

            if (best_cutback_target_score < 6.7)
            {
                best_cutback_target = nullptr;
                cutback_score = -1000.0;
            }
            else if (best_cutback_target_score < 7.9)
            {
                cutback_score -= 0.9;
            }

            if (nearest_opp_to_self < 2.0)
                cutback_score -= 2.6;
            else if (nearest_opp_to_self < 3.0)
                cutback_score -= 1.1;
        }

        double shot_score = -1000.0;
        {
            const double abs_y = std::fabs(ball.y);
            const bool close_wide_shot = (ball.x > 43.0 && abs_y < 14.0);
            const bool inside_drive_shot = (ball.x > 40.0 && abs_y < 10.0);
            const bool byline_shot = (ball.x > 47.2 && abs_y < 16.5);

            if (close_wide_shot || inside_drive_shot || byline_shot)
            {
                shot_score = 5.4;
                shot_score += (ball.x - 40.0) * 0.74;
                shot_score -= abs_y * 0.35;
                if (inside_drive_shot)
                    shot_score += 1.4;
                if (byline_shot)
                    shot_score += 0.8;
                if (ball.x < 48.0 && abs_y > 8.0)
                    shot_score -= 2.6;
                if (ball.x < 46.0 && abs_y > 11.0)
                    shot_score -= 1.4;

                if (abs_y > 12.0 && ball.x < 47.0)
                    shot_score -= 4.0;
                if (abs_y > 15.0 && ball.x < 48.5)
                    shot_score -= 6.0;
                if (abs_y > 10.0 && ball.x < 44.0)
                    shot_score -= 3.0;

                int central_blockers = 0;
                for (auto o = wm.opponentsFromBall().begin();
                     o != wm.opponentsFromBall().end();
                     ++o)
                {
                    if (!(*o))
                        continue;
                    const Vector2D op = (*o)->pos();
                    if (op.x > ball.x && op.x < 52.5 && std::fabs(op.y - ball.y * 0.5) < 3.5)
                    {
                        ++central_blockers;
                    }
                }

                shot_score -= central_blockers * 1.35;

                if (nearest_opp_to_self < 2.0)
                    shot_score -= 1.4;
                else if (nearest_opp_to_self < 3.0)
                    shot_score -= 0.6;

                if (goalie)
                {
                    const double gd = goalie->pos().dist(ball);
                    if (gd < 8.0)
                        shot_score -= 0.7;
                }

                if (shot_score > 6.0 && shot_score < 8.5 && support_count >= 2)
                {
                    shot_score -= 1.5;
                }
            }
        }

        const bool bad_angle_shot = (std::fabs(ball.y) > 12.0 && ball.x < 47.0);

        double inside_score = -1000.0;
        if (inside_pocket)
        {
            inside_score = eval_target(inside_pocket, 7.2, false);
            if (inside_pocket->pos().x > ball.x - 1.0)
                inside_score += 0.8;
            if (std::fabs(inside_pocket->pos().y) < std::fabs(ball.y) - 2.0)
            {
                inside_score += 1.1;
            }
            if (ball.x < 44.0)
                inside_score += 0.8;

            const int inside_pressure = nearby_opponents(inside_pocket->pos(), 2.5);
            if (inside_pressure == 0)
                inside_score += 1.0;
            else if (inside_pressure >= 2)
                inside_score -= 1.0;

            if (nearest_opp_to_self < 2.0)
                inside_score -= 0.8;
        }

        double recycle_score = -1000.0;
        if (recycle)
        {
            recycle_score = eval_target(recycle, 5.9, false);

            if (nearest_opp_to_self < 2.0)
                recycle_score += 2.4;
            else if (nearest_opp_to_self < 3.0)
                recycle_score += 1.0;

            int crowded_box = 0;
            for (auto o = wm.opponentsFromBall().begin();
                 o != wm.opponentsFromBall().end();
                 ++o)
            {
                if (!(*o))
                    continue;
                const Vector2D op = (*o)->pos();
                if (op.x > 34.0 && std::fabs(op.y) < 12.0)
                    ++crowded_box;
            }

            recycle_score += crowded_box * 0.8;

            const Vector2D rpos = recycle->pos();
            if (rpos.x < ball.x - 3.0)
                recycle_score -= 2.3;
            if (lane_block_penalty(ball, rpos) > 1.5)
                recycle_score -= 3.0;
            if (nearby_opponents(rpos, 3.0) > 0)
                recycle_score -= 2.0;
            if (ball.x > 43.0 && std::fabs(ball.y) < 16.0)
                recycle_score -= 3.8;
            if (ball.x > 46.0)
                recycle_score -= 3.0;
        }

        double attack_box_score = -1000.0;
        {
            const double abs_y = std::fabs(ball.y);
            const bool in_attack_box_zone =
                (ball.x > 39.0 && ball.x < 49.8 && abs_y > 5.5 && abs_y < 18.0);

            if (in_attack_box_zone)
            {
                attack_box_score = 5.5;
                if (ball.x < 47.8)
                    attack_box_score += 1.5;
                if (abs_y > 7.0 && abs_y < 14.0)
                    attack_box_score += 1.0;
                if (nearest_opp_to_self > 3.2)
                    attack_box_score += 1.8;
                else if (nearest_opp_to_self > 2.5)
                    attack_box_score += 1.0;
                else
                    attack_box_score -= 2.1;

                if (cutback_score > 8.0)
                    attack_box_score -= 2.6;
                else if (cutback_score < 7.0)
                    attack_box_score += 0.8;

                if (shot_score > 6.0 && shot_score < 8.3)
                    attack_box_score += 1.2;
                if (ball.x > 48.5)
                    attack_box_score -= 2.0;
            }
        }

        double hold_score = -1000.0;
        {
            const double abs_y = std::fabs(ball.y);
            if (ball.x > 40.0 && ball.x < 49.2 && abs_y > 5.0 && abs_y < 18.5)
            {
                hold_score = 6.2;

                if (nearest_opp_to_self > 2.0 && nearest_opp_to_self < 4.5)
                {
                    hold_score += 1.8;
                }
                else if (nearest_opp_to_self <= 2.0)
                {
                    hold_score -= 1.5;
                }

                if (cutback_score > 6.0 && cutback_score < 8.0)
                    hold_score += 1.2;
                if (inside_score > 5.8 && inside_score < 7.8)
                    hold_score += 1.0;
                if (ball.x > 45.0 && shot_score > 6.0 && shot_score < 8.4)
                {
                    hold_score += 1.1;
                }
                if (ball.x > 46.0 && abs_y < 13.0)
                    hold_score += 0.7;
                if (support_count >= 2)
                    hold_score += 1.4;
                if (shot_score > 8.5)
                    hold_score -= 2.0;
            }
        }

        Vector2D improve_target;
        double improve_space_score = -1000.0;
        const bool can_improve_more =
            can_attack_more_space(ball, &improve_target, &improve_space_score);

        if (can_improve_more)
        {
            improve_space_score += 2.0;
            if (nearest_opp_to_self < 2.2)
                improve_space_score -= 2.0;
            if (ball.x > 48.2)
                improve_space_score -= 1.5;
            if (support_count >= 2)
                improve_space_score += 0.4;
        }

        if (bad_angle_shot)
        {
            if (can_improve_more)
            {
                improve_space_score += 2.5;
            }

            attack_box_score += 1.5;
        }

        const bool elite_cutback_now = (cutback_score > 8.2);
        const bool elite_shot_now = (!bad_angle_shot && shot_score > 8.6);
        const bool elite_inside_now = (inside_score > 7.8);

        // In the late box, avoid rushing low-value shots when a pass or pause is better.
        if (ball.x > 46.0
            && std::fabs(ball.y) < 14.0
            && !elite_cutback_now
            && !elite_shot_now
            && nearest_opp_to_self > 1.8)
        {
            if (far_post && far_score > 7.0 && !lane_is_dead(ball, far_post->pos()))
            {
                Vector2D target_pos = far_post->pos();
                if (far_post->vel().r() > 0.2)
                    target_pos += far_post->vel() * 0.3;
                const double dist = ball.dist(target_pos);
                const double kick_speed = std::min(2.4, std::max(1.5, 1.1 + dist * 0.04));

                if (Body_SmartKick(target_pos, kick_speed, 1.0, 2).execute(agent))
                {
                    agent->debugClient().addMessage("Cutback");
                    agent->setNeckAction(new Neck_ScanField());
                    return;
                }
            }

            if (inside_pocket && inside_score > 7.0)
            {
                Vector2D target_pos = inside_pocket->pos();
                if (inside_pocket->vel().r() > 0.2)
                {
                    target_pos += inside_pocket->vel() * 0.3;
                }
                const double dist = ball.dist(target_pos);
                const double kick_speed = std::min(2.3, std::max(1.4, 1.0 + dist * 0.05));

                if (Body_SmartKick(target_pos, kick_speed, 1.0, 2).execute(agent))
                {
                    agent->debugClient().addMessage("InsidePass");
                    agent->setNeckAction(new Neck_ScanField());
                    return;
                }
            }

            if (hold_score > 7.0)
            {
                Body_HoldBall().execute(agent);
                agent->debugClient().addMessage("Hold");
                agent->setNeckAction(new Neck_ScanField());
                return;
            }
        }

        if (elite_cutback_now && best_cutback_target)
        {
            Vector2D target_pos = best_cutback_target->pos();
            if (best_cutback_target->vel().r() > 0.2)
            {
                target_pos += best_cutback_target->vel() * 0.4;
            }

            const double dist = ball.dist(target_pos);
            const double kick_speed = std::min(2.5, std::max(1.5, 1.2 + dist * 0.04));

            if (Body_SmartKick(target_pos, kick_speed, 1.0, 2).execute(agent))
            {
                agent->debugClient().addMessage("Cutback");
                agent->setNeckAction(new Neck_ScanField());
                return;
            }
        }

        if (elite_shot_now)
        {
            double target_y = (ball.y > 0.0 ? -7.0 : 7.0);
            if (goalie)
            {
                if (goalie->pos().y > 1.0)
                    target_y = -7.5;
                else if (goalie->pos().y < -1.0)
                    target_y = 7.5;
                else
                    target_y = (ball.y > 0.0 ? -6.8 : 6.8);
            }

            const Vector2D goal_target(52.5, target_y);
            double shot_speed = 2.95;

            if (Body_SmartKick(goal_target, shot_speed, 1.0, 2).execute(agent))
            {
                agent->debugClient().addMessage("Shot");
                agent->setNeckAction(new Neck_ScanField());
                return;
            }
        }

        if (elite_inside_now && inside_pocket)
        {
            Vector2D target_pos = inside_pocket->pos();
            if (inside_pocket->vel().r() > 0.2)
            {
                target_pos += inside_pocket->vel() * 0.4;
            }

            const double dist = ball.dist(target_pos);
            const double kick_speed = std::min(2.4, std::max(1.4, 1.1 + dist * 0.05));

            if (Body_SmartKick(target_pos, kick_speed, 1.0, 2).execute(agent))
            {
                agent->debugClient().addMessage("InsidePass");
                agent->setNeckAction(new Neck_ScanField());
                return;
            }
        }

        enum ComposureAction
        {
            ACTION_NONE,
            ACTION_HOLD,
            ACTION_ATTACK_BOX,
            ACTION_INSIDE_PASS,
            ACTION_CUTBACK,
            ACTION_RECYCLE,
            ACTION_SHOT
        };

        ComposureAction best_action = ACTION_NONE;
        double best_action_score = -1000.0;

        auto consider_action = [&](const ComposureAction a, const double s)
        {
            if (s > best_action_score)
            {
                best_action_score = s;
                best_action = a;
            }
        };

        consider_action(ACTION_HOLD, hold_score);
        consider_action(ACTION_ATTACK_BOX, attack_box_score);
        consider_action(ACTION_INSIDE_PASS, inside_score);
        consider_action(ACTION_CUTBACK, cutback_score);
        consider_action(ACTION_RECYCLE, recycle_score);
        consider_action(ACTION_SHOT, shot_score);

        if (can_improve_more
            && improve_space_score > 6.0
            && !elite_cutback_now
            && !elite_shot_now
            && nearest_opp_to_self > 2.2)
        {
            if (Body_Dribble2008(improve_target,
                                 1.0,
                                 ServerParam::i().maxDashPower(),
                                 4)
                    .execute(agent))
            {
                agent->debugClient().addMessage("AttackSpace");
                agent->setNeckAction(new Neck_ScanField());
                return;
            }
        }

        if (best_action == ACTION_HOLD && best_action_score > 6.4)
        {
            Body_HoldBall().execute(agent);
            agent->debugClient().addMessage("Hold");
            agent->setNeckAction(new Neck_ScanField());
            return;
        }

        if (best_action == ACTION_ATTACK_BOX && best_action_score > 6.8)
        {
            const double side = (ball.y > 0.0 ? 1.0 : -1.0);
            Vector2D drive_target(ball.x + 3.2, ball.y - side * 2.4);
            drive_target.x = std::min(50.8, drive_target.x);
            drive_target.y = min_max(-ServerParam::i().pitchHalfWidth() + 1.0,
                                     drive_target.y,
                                     ServerParam::i().pitchHalfWidth() - 1.0);

            if (Body_Dribble2008(drive_target,
                                 1.0,
                                 ServerParam::i().maxDashPower(),
                                 4)
                    .execute(agent))
            {
                agent->debugClient().addMessage("AttackSpace");
                agent->setNeckAction(new Neck_ScanField());
                return;
            }
        }

        if (best_action == ACTION_INSIDE_PASS && inside_pocket && best_action_score > 6.7)
        {
            Vector2D target_pos = inside_pocket->pos();
            if (inside_pocket->vel().r() > 0.2)
            {
                target_pos += inside_pocket->vel() * 0.4;
            }

            const double dist = ball.dist(target_pos);
            const double kick_speed = std::min(2.4, std::max(1.4, 1.1 + dist * 0.05));

            if (Body_SmartKick(target_pos, kick_speed, 1.0, 2).execute(agent))
            {
                agent->debugClient().addMessage("InsidePass");
                agent->setNeckAction(new Neck_ScanField());
                return;
            }
        }

        if (best_action == ACTION_CUTBACK && best_cutback_target && best_action_score > 7.0)
        {
            Vector2D target_pos = best_cutback_target->pos();
            if (best_cutback_target->vel().r() > 0.2)
            {
                target_pos += best_cutback_target->vel() * 0.4;
            }

            const double dist = ball.dist(target_pos);
            const double kick_speed = std::min(2.5, std::max(1.5, 1.2 + dist * 0.04));

            if (Body_SmartKick(target_pos, kick_speed, 1.0, 2).execute(agent))
            {
                agent->debugClient().addMessage("Cutback");
                agent->setNeckAction(new Neck_ScanField());
                return;
            }
        }

        if (best_action == ACTION_RECYCLE && recycle && best_action_score > 6.0)
        {
            const Vector2D target_pos = recycle->pos();
            const double dist = ball.dist(target_pos);
            const double kick_speed = std::min(2.3, std::max(1.4, 1.0 + dist * 0.04));

            if (Body_SmartKick(target_pos, kick_speed, 1.0, 2).execute(agent))
            {
                agent->debugClient().addMessage("Recycle");
                agent->setNeckAction(new Neck_ScanField());
                return;
            }
        }

        if (best_action == ACTION_SHOT && best_action_score > 8.4 && !bad_angle_shot)
        {
            double target_y = (ball.y > 0.0 ? -7.0 : 7.0);
            if (goalie)
            {
                if (goalie->pos().y > 1.0)
                    target_y = -7.5;
                else if (goalie->pos().y < -1.0)
                    target_y = 7.5;
            }

            const Vector2D goal_target(52.5, target_y);
            double shot_speed = 2.9;

            if (Body_SmartKick(goal_target, shot_speed, 1.0, 2).execute(agent))
            {
                agent->debugClient().addMessage("Shot");
                agent->setNeckAction(new Neck_ScanField());
                return;
            }
        }

        if (hold_score > 5.8 && nearest_opp_to_self > 1.8)
        {
            Body_HoldBall().execute(agent);
            agent->debugClient().addMessage("Hold");
            agent->setNeckAction(new Neck_ScanField());
            return;
        }
    }

    if (Bhv_PlannedAction().execute(agent))
    {
        return;
    }
}

void RoleSideForward::doMove(PlayerAgent *agent)
{
    Bhv_BasicMove().execute(agent);
}
