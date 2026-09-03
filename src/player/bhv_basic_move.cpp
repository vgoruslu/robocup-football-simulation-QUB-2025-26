// -*-c++-*-

/*
 *Copyright:

 Copyright (C) Hidehisa AKIYAMA

 This code is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; either version 3, or (at your option)
 any later version.

 This code is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this code; see the file COPYING.  If not, write to
 the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.

 *EndCopyright:
 */

/////////////////////////////////////////////////////////////////////

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "bhv_basic_move.h"

#include "strategy.h"

#include "bhv_basic_tackle.h"

#include "basic_actions/basic_actions.h"
#include "basic_actions/body_go_to_point.h"
#include "basic_actions/body_intercept.h"
#include "basic_actions/body_turn_to_point.h"
#include "basic_actions/neck_turn_to_ball_or_scan.h"
#include "basic_actions/neck_turn_to_low_conf_teammate.h"

#include <rcsc/player/player_agent.h>
#include <rcsc/player/debug_client.h>
#include <rcsc/player/intercept_table.h>

#include <rcsc/common/server_param.h>

#include "neck_offensive_intercept_neck.h"

#include <algorithm>
#include <vector>
#include <cmath>

using namespace rcsc;

namespace
{
    int s_last_block_cycle = -1;
    Vector2D s_last_block_pos = Vector2D::INVALIDATED;

    AngleDeg get_block_dribble_direction(const Vector2D &dribble_pos)
    {
        AngleDeg best_dir(-180.0);
        double best_score = -1.0e9;
        const double dist = 10.0;

        for (double dir = -180.0; dir < 180.0; dir += 10.0)
        {
            Vector2D target = dribble_pos + Vector2D::polar2vector(dist, AngleDeg(dir));

            if (target.absX() > ServerParam::i().pitchHalfLength())
                continue;
            if (target.absY() > ServerParam::i().pitchHalfWidth())
                continue;

            // Prefer directions that move the attack toward our goal.
            double score = -target.x + std::max(0.0, 40.0 - target.dist(Vector2D(-50.0, 0.0)));

            if (score > best_score)
            {
                best_score = score;
                best_dir = AngleDeg(dir);
            }
        }

        return best_dir;
    }

    std::vector<int> get_blockers(const PlayerAgent *agent)
    {
        const WorldModel &wm = agent->world();
        const int opp_min = wm.interceptTable().opponentStep();
        const Vector2D ball_inertia = wm.ball().inertiaPoint(opp_min);

        std::vector<int> blockers;

        for (const AbstractPlayerObject *tm : wm.ourPlayers())
        {
            if (!tm)
                continue;
            if (tm->isGhost())
                continue;
            if (tm->goalie())
                continue;
            if (tm->isTackling())
                continue;
            if (tm->pos().dist(ball_inertia) > 40.0)
                continue;

            const Vector2D home_pos = Strategy::i().getPosition(tm->unum());
            if (home_pos.dist(ball_inertia) > 40.0)
                continue;

            // Avoid dragging the back line too far out of shape.
            if (tm->unum() <= 5)
            {
                double defense_line_x = 0.0;
                for (int i = 2; i <= 5; ++i)
                {
                    const AbstractPlayerObject *def_tm = wm.ourPlayer(i);
                    if (def_tm && def_tm->unum() > 0)
                    {
                        if (def_tm->pos().x < defense_line_x)
                        {
                            defense_line_x = def_tm->pos().x;
                        }
                    }
                }

                if (ball_inertia.x > -30.0 && ball_inertia.x > home_pos.x + 10.0 && ball_inertia.x > defense_line_x + 10.0)
                {
                    continue;
                }
            }

            blockers.push_back(tm->unum());
        }

        return blockers;
    }

    std::pair<int, Vector2D> get_best_blocker(const PlayerAgent *agent,
                                              const std::vector<int> &blockers)
    {
        const WorldModel &wm = agent->world();
        const int opp_min = wm.interceptTable().opponentStep();
        Vector2D ball_inertia = wm.ball().inertiaPoint(opp_min);
        const double dribble_speed = 0.7;

        for (int cycle = opp_min + 1; cycle <= opp_min + 40; ++cycle)
        {
            const AngleDeg dir = get_block_dribble_direction(ball_inertia);
            ball_inertia += Vector2D::polar2vector(dribble_speed, dir);

            for (int unum : blockers)
            {
                const AbstractPlayerObject *tm = wm.ourPlayer(unum);
                if (!tm)
                    continue;

                const Vector2D tm_pos = tm->pos() + tm->vel();
                const double dist = ball_inertia.dist(tm_pos);
                const int dash_step = tm->playerTypePtr()->cyclesToReachDistance(dist);

                if (dash_step <= cycle)
                {
                    return std::make_pair(unum, ball_inertia);
                }
            }
        }

        return std::make_pair(0, Vector2D::INVALIDATED);
    }

    bool do_block_move(PlayerAgent *agent)
    {
        const WorldModel &wm = agent->world();

        // Only consider blocking when the opponent is likely first to the ball.
        if (std::min(wm.interceptTable().selfStep(),
                     wm.interceptTable().teammateStep()) < wm.interceptTable().opponentStep())
        {
            s_last_block_cycle = -1;
            s_last_block_pos = Vector2D::INVALIDATED;
            return false;
        }

        // Keep this behaviour for defensive areas only.
        if (wm.ball().pos().x > -10.0)
        {
            s_last_block_cycle = -1;
            s_last_block_pos = Vector2D::INVALIDATED;
            return false;
        }

        std::vector<int> blockers = get_blockers(agent);
        if (blockers.empty())
        {
            s_last_block_cycle = -1;
            s_last_block_pos = Vector2D::INVALIDATED;
            return false;
        }

        const int self_unum = wm.self().unum();
        if (std::find(blockers.begin(), blockers.end(), self_unum) == blockers.end())
        {
            s_last_block_cycle = -1;
            s_last_block_pos = Vector2D::INVALIDATED;
            return false;
        }

        const std::pair<int, Vector2D> best = get_best_blocker(agent, blockers);
        if (best.first != self_unum || !best.second.isValid())
        {
            s_last_block_cycle = -1;
            s_last_block_pos = Vector2D::INVALIDATED;
            return false;
        }

        Vector2D target_point = best.second;

        double safe_dist = 2.0;
        if (wm.self().pos().dist(target_point) > 15.0)
        {
            safe_dist = 5.0;
        }

        if (s_last_block_pos.isValid() && s_last_block_cycle > wm.time().cycle() - 5 && target_point.dist(s_last_block_pos) < safe_dist)
        {
            target_point = s_last_block_pos;
        }
        else
        {
            s_last_block_cycle = wm.time().cycle();
            s_last_block_pos = target_point;
        }

        agent->debugClient().addMessage("BasicBlock");
        agent->debugClient().setTarget(target_point);
        agent->debugClient().addCircle(target_point, 0.5);

        if (!Body_GoToPoint(target_point,
                            0.5,
                            100.0,
                            -1.0,
                            100,
                            false,
                            25.0,
                            1.0,
                            false)
                 .execute(agent))
        {
            Body_TurnToPoint(target_point).execute(agent);
        }

        if (wm.kickableOpponent() && wm.ball().distFromSelf() < 18.0)
        {
            agent->setNeckAction(new Neck_TurnToBall());
        }
        else
        {
            agent->setNeckAction(new Neck_TurnToBallOrScan(0));
        }

        return true;
    }

} // unnamed namespace

/*-------------------------------------------------------------------*/
/*!

 */
bool Bhv_BasicMove::execute(PlayerAgent *agent)
{
    const WorldModel &wm = agent->world();

    double doTackleProb = 0.8;
    if (wm.ball().pos().x < 0.0)
    {
        doTackleProb = 0.5;
    }

    if (Bhv_BasicTackle(doTackleProb, 80.0).execute(agent))
    {
        return true;
    }

    const int self_min = wm.interceptTable().selfStep();
    const int mate_min = wm.interceptTable().teammateStep();
    const int opp_min = wm.interceptTable().opponentStep();

    int role = Strategy::i().roleNumber(wm.self().unum());
    int pressing = 13;

    if (role >= 6 && role <= 8 && wm.ball().pos().x > -30.0 && wm.self().pos().x < 10.0)
    {
        pressing = 7;
    }

    if (std::fabs(wm.ball().pos().y) > 22.0 && wm.ball().pos().x < 0.0 && wm.ball().pos().x > -36.5 && (role == 4 || role == 5))
    {
        pressing = 23;
    }

    if (!wm.kickableTeammate() && (self_min <= 3 || (self_min <= mate_min && self_min < opp_min + pressing)))
    {
        Body_Intercept().execute(agent);
        agent->setNeckAction(new Neck_OffensiveInterceptNeck());

        return true;
    }

    if (do_block_move(agent))
    {
        return true;
    }

    const Vector2D target_point = Strategy::i().getPosition(wm.self().unum());
    const double dash_power = Strategy::get_normal_dash_power(wm);

    double dist_thr = wm.ball().distFromSelf() * 0.1;
    if (dist_thr < 1.0)
        dist_thr = 1.0;

    agent->debugClient().addMessage("BasicMove%.0f", dash_power);
    agent->debugClient().setTarget(target_point);
    agent->debugClient().addCircle(target_point, dist_thr);

    if (!Body_GoToPoint(target_point, dist_thr, dash_power).execute(agent))
    {
        Body_TurnToBall().execute(agent);
    }

    if (wm.kickableOpponent() && wm.ball().distFromSelf() < 18.0)
    {
        agent->setNeckAction(new Neck_TurnToBall());
    }
    else
    {
        agent->setNeckAction(new Neck_TurnToBallOrScan(0));
    }

    return true;
}
