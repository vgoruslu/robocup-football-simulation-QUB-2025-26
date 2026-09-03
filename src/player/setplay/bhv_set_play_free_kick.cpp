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

#include "bhv_set_play_free_kick.h"

#include "strategy.h"

#include "bhv_set_play.h"
#include "bhv_prepare_set_play_kick.h"
#include "bhv_go_to_placed_ball.h"

#include "intention_wait_after_set_play_kick.h"

#include "planner/bhv_planned_action.h"

#include "basic_actions/basic_actions.h"
#include "basic_actions/body_go_to_point.h"
#include "basic_actions/body_kick_one_step.h"
#include "basic_actions/body_clear_ball.h"
#include "basic_actions/body_pass.h"
#include "basic_actions/neck_scan_field.h"
#include "basic_actions/neck_turn_to_ball_or_scan.h"

#include <rcsc/player/player_agent.h>
#include <rcsc/player/debug_client.h>
#include <rcsc/player/say_message_builder.h>

#include <rcsc/common/server_param.h>
#include <rcsc/geom/circle_2d.h>
#include <rcsc/math_util.h>

using namespace rcsc;

namespace
{
    bool isInsidePitch(const Vector2D &p)
    {
        return p.absX() < ServerParam::i().pitchHalfLength() - 1.0 && p.absY() < ServerParam::i().pitchHalfWidth() - 1.0;
    }

    double distToSegment(const Vector2D &p,
                         const Vector2D &a,
                         const Vector2D &b)
    {
        Vector2D ab = b - a;
        double ab2 = ab.r2();

        if (ab2 < 1.0e-6)
            return p.dist(a);

        double t = ((p - a).innerProduct(ab)) / ab2;
        t = min_max(0.0, t, 1.0);

        Vector2D proj = a + ab * t;
        return p.dist(proj);
    }

    bool passLaneBlocked(const WorldModel &wm,
                         const Vector2D &from,
                         const Vector2D &to)
    {
        for (const PlayerObject *opp : wm.opponentsFromBall())
        {
            if (!opp)
                continue;
            if (opp->posCount() > 8)
                continue;

            const Vector2D opp_pos = opp->pos();

            if (opp_pos.dist(from) > from.dist(to) + 3.0)
                continue;

            const double line_dist = distToSegment(opp_pos, from, to);
            const double lane_thr = (from.dist(opp_pos) < 10.0 ? 2.2 : 1.6);

            if (line_dist < lane_thr)
            {
                return true;
            }
        }

        return false;
    }

    double nearestOpponentDistance(const WorldModel &wm,
                                   const Vector2D &point)
    {
        double best = 1000.0;

        for (const PlayerObject *opp : wm.opponentsFromBall())
        {
            if (!opp)
                continue;
            if (opp->posCount() > 10)
                continue;

            best = std::min(best, opp->pos().dist(point));
        }

        return best;
    }

    double secondNearestOpponentDistance(const WorldModel &wm,
                                         const Vector2D &point)
    {
        double best1 = 1000.0;
        double best2 = 1000.0;

        for (const PlayerObject *opp : wm.opponentsFromBall())
        {
            if (!opp)
                continue;
            if (opp->posCount() > 10)
                continue;

            const double d = opp->pos().dist(point);

            if (d < best1)
            {
                best2 = best1;
                best1 = d;
            }
            else if (d < best2)
            {
                best2 = d;
            }
        }

        return best2;
    }

    double getOneStepSetPlaySpeed(const WorldModel &wm,
                                  const Vector2D &target,
                                  const int desired_steps)
    {
        const double max_ball_speed = wm.self().kickRate() * ServerParam::i().maxPower();
        const double dist = wm.ball().pos().dist(target);

        double speed = calc_first_term_geom_series(dist,
                                                   ServerParam::i().ballDecay(),
                                                   desired_steps);
        return std::min(speed, max_ball_speed);
    }

    bool doSmartKick(PlayerAgent *agent,
                     const Vector2D &target,
                     const int desired_steps,
                     const char *debug_msg)
    {
        const WorldModel &wm = agent->world();
        const double speed = getOneStepSetPlaySpeed(wm, target, desired_steps);

        agent->debugClient().addMessage(debug_msg);
        agent->debugClient().setTarget(target);

        Body_KickOneStep(target, speed).execute(agent);
        agent->setNeckAction(new Neck_ScanField());
        return true;
    }

    bool isDefensiveFreeKick(const WorldModel &wm)
    {
        return wm.ball().pos().x < -20.0;
    }

    bool isMiddleFreeKick(const WorldModel &wm)
    {
        return wm.ball().pos().x >= -20.0 && wm.ball().pos().x < 20.0;
    }

    bool isAttackingWideFreeKick(const WorldModel &wm)
    {
        return wm.ball().pos().x >= 20.0 && std::fabs(wm.ball().pos().y) > 12.0;
    }

    bool isAttackingCentralFreeKick(const WorldModel &wm)
    {
        return wm.ball().pos().x >= 20.0 && std::fabs(wm.ball().pos().y) <= 12.0;
    }

    double scoreFreeKickReceiver(const WorldModel &wm,
                                 const Vector2D &target,
                                 const bool require_forward,
                                 const double max_pass_dist)
    {
        const Vector2D ball_pos = wm.ball().pos();

        if (!isInsidePitch(target))
            return -1.0e9;

        const double pass_dist = ball_pos.dist(target);
        if (pass_dist < 4.0 || pass_dist > max_pass_dist)
            return -1.0e9;

        const double dx = target.x - ball_pos.x;
        if (require_forward && dx < 1.5)
            return -1.0e9;

        if (passLaneBlocked(wm, ball_pos, target))
            return -1.0e9;

        const double nearest_opp = nearestOpponentDistance(wm, target);
        const double second_opp = secondNearestOpponentDistance(wm, target);

        if (nearest_opp < 4.0)
            return -1.0e9;
        if (second_opp < 2.5)
            return -1.0e9;

        if (target.x < -25.0 && std::fabs(target.y) < 14.0 && nearest_opp < 5.0)
        {
            return -1.0e9;
        }

        double score = 0.0;
        score += nearest_opp * 6.0;
        score += second_opp * 3.0;
        score -= std::fabs(pass_dist - 9.0) * 0.4;
        score -= std::fabs(target.y - ball_pos.y) * 0.08;

        if (dx > 0.0)
        {
            score += std::min(dx, 10.0) * 0.8;
        }

        return score;
    }

    const PlayerObject *selectBestFreeKickReceiver(const WorldModel &wm,
                                                   Vector2D *target,
                                                   const bool require_forward,
                                                   const double max_pass_dist,
                                                   const double min_score)
    {
        const PlayerObject *best = nullptr;
        Vector2D best_point;
        double best_score = -1.0e9;

        for (const PlayerObject *tm : wm.teammatesFromBall())
        {
            if (!tm)
                continue;
            if (tm->unum() == wm.self().unum())
                continue;
            if (tm->goalie())
                continue;
            if (tm->posCount() > 8)
                continue;

            Vector2D tp = tm->inertiaFinalPoint();
            double score = scoreFreeKickReceiver(wm,
                                                 tp,
                                                 require_forward,
                                                 max_pass_dist);

            if (score > best_score)
            {
                best_score = score;
                best = tm;
                best_point = tp;
            }
        }

        if (!best || best_score < min_score)
            return nullptr;

        if (target)
            *target = best_point;
        return best;
    }

    const PlayerObject *selectWideFreeKickTarget(const WorldModel &wm,
                                                 Vector2D *target)
    {
        const Vector2D ball_pos = wm.ball().pos();

        const PlayerObject *best = nullptr;
        double best_score = -1.0e9;

        for (const PlayerObject *tm : wm.teammatesFromBall())
        {
            if (!tm)
                continue;
            if (tm->unum() == wm.self().unum())
                continue;
            if (tm->posCount() > 8)
                continue;

            Vector2D tp = tm->inertiaFinalPoint();

            if (!isInsidePitch(tp))
                continue;
            if (tp.x < 34.0)
                continue;
            if (std::fabs(tp.y) > 20.0)
                continue;

            const double pass_dist = ball_pos.dist(tp);
            if (pass_dist < 8.0 || pass_dist > 28.0)
                continue;

            const double opp_dist = nearestOpponentDistance(wm, tp);
            if (opp_dist < 2.2)
                continue;

            if (passLaneBlocked(wm, ball_pos, tp))
                continue;

            double score = 0.0;
            score += tp.x * 3.0;
            score += opp_dist * 2.5;
            score -= std::fabs(tp.y) * 0.25;
            score -= std::fabs(pass_dist - 16.0) * 0.2;

            if (score > best_score)
            {
                best_score = score;
                best = tm;
                if (target)
                    *target = tp;
            }
        }

        return best;
    }

    bool shouldShootDirectFreeKick(const WorldModel &wm)
    {
        const Vector2D ball = wm.ball().pos();

        return ball.x > 30.0 && std::fabs(ball.y) < 10.0;
    }

}

bool Bhv_SetPlayFreeKick::execute(PlayerAgent *agent)
{
    if (Bhv_SetPlay::is_kicker(agent))
    {
        doKick(agent);
    }
    else
    {
        doMove(agent);
    }

    return true;
}

void Bhv_SetPlayFreeKick::doKick(PlayerAgent *agent)
{
    if (Bhv_GoToPlacedBall(0.0).execute(agent))
    {
        return;
    }

    if (doKickWait(agent))
    {
        return;
    }

    const WorldModel &wm = agent->world();

    if (Bhv_PlannedAction().execute(agent))
    {
        agent->debugClient().addMessage("FreeKick:Plan");
        agent->setIntention(new IntentionWaitAfterSetPlayKick());
        return;
    }

    Vector2D target_point;

    if (isDefensiveFreeKick(wm))
    {
        if (selectBestFreeKickReceiver(wm, &target_point, false, 16.0, 32.0))
        {
            doSmartKick(agent, target_point, 2, "FreeKick:DefSafe");
            return;
        }

        if (selectBestFreeKickReceiver(wm, &target_point, false, 22.0, 28.0))
        {
            doSmartKick(agent, target_point, 2, "FreeKick:DefRecycle");
            return;
        }

        if ((wm.ball().angleFromSelf() - wm.self().body()).abs() > 1.5)
        {
            agent->debugClient().addMessage("FreeKick:DefTurn");
            Body_TurnToBall().execute(agent);
            agent->setNeckAction(new Neck_ScanField());
            return;
        }

        agent->debugClient().addMessage("FreeKick:DefClear");
        Body_ClearBall().execute(agent);
        agent->setNeckAction(new Neck_ScanField());
        return;
    }

    if (isMiddleFreeKick(wm))
    {
        if (selectBestFreeKickReceiver(wm, &target_point, true, 16.0, 34.0))
        {
            doSmartKick(agent, target_point, 2, "FreeKick:MidForward");
            return;
        }

        if (selectBestFreeKickReceiver(wm, &target_point, false, 18.0, 30.0))
        {
            doSmartKick(agent, target_point, 2, "FreeKick:MidRecycle");
            return;
        }

        if ((wm.ball().angleFromSelf() - wm.self().body()).abs() > 1.5)
        {
            agent->debugClient().addMessage("FreeKick:MidTurn");
            Body_TurnToBall().execute(agent);
            agent->setNeckAction(new Neck_ScanField());
            return;
        }

        agent->debugClient().addMessage("FreeKick:MidClear");
        Body_ClearBall().execute(agent);
        agent->setNeckAction(new Neck_ScanField());
        return;
    }

    if (isAttackingWideFreeKick(wm))
    {
        if (selectWideFreeKickTarget(wm, &target_point))
        {
            doSmartKick(agent, target_point, 3, "FreeKick:WideDeliver");
            return;
        }

        if (selectBestFreeKickReceiver(wm, &target_point, false, 12.0, 28.0))
        {
            doSmartKick(agent, target_point, 2, "FreeKick:WideShort");
            return;
        }

        Vector2D danger(ServerParam::i().pitchHalfLength() - 6.0, 0.0);
        doSmartKick(agent, danger, 3, "FreeKick:WideForce");
        return;
    }

    if (isAttackingCentralFreeKick(wm))
    {
        if (shouldShootDirectFreeKick(wm))
        {
            Vector2D goal_target(ServerParam::i().pitchHalfLength(), 0.0);
            doSmartKick(agent, goal_target, 3, "FreeKick:DirectShot");
            return;
        }

        if (selectBestFreeKickReceiver(wm, &target_point, true, 14.0, 30.0))
        {
            doSmartKick(agent, target_point, 2, "FreeKick:CentralSlip");
            return;
        }

        if (selectBestFreeKickReceiver(wm, &target_point, false, 12.0, 28.0))
        {
            doSmartKick(agent, target_point, 2, "FreeKick:CentralShort");
            return;
        }

        Vector2D danger(ServerParam::i().pitchHalfLength() - 4.0, 0.0);
        doSmartKick(agent, danger, 3, "FreeKick:CentralForce");
        return;
    }

    if ((wm.ball().angleFromSelf() - wm.self().body()).abs() > 1.5)
    {
        agent->debugClient().addMessage("FreeKick:Clear:TurnToBall");
        Body_TurnToBall().execute(agent);
        agent->setNeckAction(new Neck_ScanField());
        return;
    }

    agent->debugClient().addMessage("FreeKick:Clear");
    Body_ClearBall().execute(agent);
    agent->setNeckAction(new Neck_ScanField());
}

bool Bhv_SetPlayFreeKick::doKickWait(PlayerAgent *agent)
{
    const WorldModel &wm = agent->world();

    const int real_set_play_count = static_cast<int>(wm.time().cycle() - wm.lastSetPlayStartTime().cycle());

    if (real_set_play_count >= ServerParam::i().dropBallTime() - 5)
    {
        return false;
    }

    const Vector2D face_point(40.0, 0.0);
    const AngleDeg face_angle = (face_point - wm.self().pos()).th();

    if (wm.time().stopped() != 0)
    {
        Body_TurnToPoint(face_point).execute(agent);
        agent->setNeckAction(new Neck_ScanField());
        return true;
    }

    if (Bhv_SetPlay::is_delaying_tactics_situation(agent))
    {
        agent->debugClient().addMessage("FreeKick:Delaying");
        Body_TurnToPoint(face_point).execute(agent);
        agent->setNeckAction(new Neck_ScanField());
        return true;
    }

    if (wm.teammatesFromBall().empty())
    {
        agent->debugClient().addMessage("FreeKick:NoTeammate");
        Body_TurnToPoint(face_point).execute(agent);
        agent->setNeckAction(new Neck_ScanField());
        return true;
    }

    if (wm.getSetPlayCount() <= 3)
    {
        agent->debugClient().addMessage("FreeKick:Wait%d", wm.getSetPlayCount());
        Body_TurnToPoint(face_point).execute(agent);
        agent->setNeckAction(new Neck_ScanField());
        return true;
    }

    if (wm.getSetPlayCount() >= 15 && wm.seeTime() == wm.time() && wm.self().stamina() > ServerParam::i().staminaMax() * 0.6)
    {
        return false;
    }

    if ((face_angle - wm.self().body()).abs() > 5.0)
    {
        agent->debugClient().addMessage("FreeKick:Turn");
        Body_TurnToPoint(face_point).execute(agent);
        agent->setNeckAction(new Neck_ScanField());
        return true;
    }

    if (wm.seeTime() != wm.time() || wm.self().stamina() < ServerParam::i().staminaMax() * 0.9)
    {
        Body_TurnToBall().execute(agent);
        agent->setNeckAction(new Neck_ScanField());

        agent->debugClient().addMessage("FreeKick:Wait%d", wm.getSetPlayCount());
        return true;
    }

    return false;
}

void Bhv_SetPlayFreeKick::doMove(PlayerAgent *agent)
{
    const WorldModel &wm = agent->world();

    Vector2D target_point = Strategy::i().getPosition(wm.self().unum());

    if (wm.getSetPlayCount() > 0 && wm.self().stamina() > ServerParam::i().staminaMax() * 0.9)
    {
        const PlayerObject *nearest_opp = agent->world().getOpponentNearestToSelf(5);

        if (nearest_opp && nearest_opp->distFromSelf() < 3.0)
        {
            Vector2D add_vec = (wm.ball().pos() - target_point);
            add_vec.setLength(3.0);

            long time_val = agent->world().time().cycle() % 60;
            if (time_val < 20)
            {
            }
            else if (time_val < 40)
            {
                target_point += add_vec.rotatedVector(90.0);
            }
            else
            {
                target_point += add_vec.rotatedVector(-90.0);
            }

            target_point.x = min_max(-ServerParam::i().pitchHalfLength(),
                                     target_point.x,
                                     +ServerParam::i().pitchHalfLength());
            target_point.y = min_max(-ServerParam::i().pitchHalfWidth(),
                                     target_point.y,
                                     +ServerParam::i().pitchHalfWidth());
        }
    }

    target_point.x = std::min(target_point.x,
                              agent->world().offsideLineX() - 0.5);

    double dash_power = Bhv_SetPlay::get_set_play_dash_power(agent);
    double dist_thr = wm.ball().distFromSelf() * 0.07;
    if (dist_thr < 1.0)
        dist_thr = 1.0;

    agent->debugClient().addMessage("SetPlayMove");
    agent->debugClient().setTarget(target_point);
    agent->debugClient().addCircle(target_point, dist_thr);

    if (!Body_GoToPoint(target_point,
                        dist_thr,
                        dash_power)
             .execute(agent))
    {
        Body_TurnToBall().execute(agent);
    }

    if (wm.self().pos().dist(target_point) > std::max(wm.ball().pos().dist(target_point) * 0.2, dist_thr) + 6.0 || wm.self().stamina() < ServerParam::i().staminaMax() * 0.7)
    {
        if (!wm.self().staminaModel().capacityIsEmpty())
        {
            agent->debugClient().addMessage("Sayw");
            agent->addSayMessage(new WaitRequestMessage());
        }
    }

    agent->setNeckAction(new Neck_TurnToBallOrScan(0));
}
