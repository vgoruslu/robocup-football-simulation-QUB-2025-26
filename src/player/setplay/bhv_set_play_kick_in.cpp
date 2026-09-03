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

#include "bhv_set_play_kick_in.h"

#include "strategy.h"

#include "bhv_set_play.h"
#include "bhv_go_to_placed_ball.h"
#include "bhv_planned_action.h"

#include "intention_wait_after_set_play_kick.h"

#include "basic_actions/basic_actions.h"
#include "basic_actions/body_go_to_point.h"
#include "basic_actions/body_kick_one_step.h"
#include "basic_actions/body_advance_ball.h"
#include "basic_actions/body_pass.h"
#include "basic_actions/neck_scan_field.h"
#include "basic_actions/neck_turn_to_ball_or_scan.h"

#include <rcsc/player/player_agent.h>
#include <rcsc/player/debug_client.h>
#include <rcsc/player/say_message_builder.h>

#include <rcsc/common/server_param.h>

#include <rcsc/math_util.h>

#include <algorithm>
#include <limits>

using namespace rcsc;

namespace
{
    bool isInsidePitch(const Vector2D &p)
    {
        return p.absX() < ServerParam::i().pitchHalfLength() - 1.0 && p.absY() < ServerParam::i().pitchHalfWidth() - 1.0;
    }

    bool isCornerKickSituation(const WorldModel &wm)
    {
        const double x_thr = ServerParam::i().pitchHalfLength() - 3.0;
        const double y_thr = ServerParam::i().pitchHalfWidth() - 3.0;

        return wm.ball().pos().x > x_thr && std::fabs(wm.ball().pos().y) > y_thr;
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

    const PlayerObject *selectCornerAttackTarget(const WorldModel &wm,
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
            if (tp.x < 36.0)
                continue;
            if (std::fabs(tp.y) > 18.0)
                continue;

            const double pass_dist = ball_pos.dist(tp);
            if (pass_dist < 6.0 || pass_dist > 28.0)
                continue;

            const double opp_dist = nearestOpponentDistance(wm, tp);
            if (opp_dist < 1.8)
                continue;

            if (passLaneBlocked(wm, ball_pos, tp))
                continue;

            double score = 0.0;
            score += tp.x * 3.0;
            score -= std::fabs(tp.y) * 0.35;
            score += opp_dist * 2.5;
            score -= std::fabs(pass_dist - 14.0) * 0.3;

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

    const PlayerObject *selectShortCornerSupport(const WorldModel &wm,
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

            const double pass_dist = ball_pos.dist(tp);
            if (pass_dist < 4.0 || pass_dist > 12.0)
                continue;

            const double opp_dist = nearestOpponentDistance(wm, tp);
            if (opp_dist < 3.0)
                continue;

            if (passLaneBlocked(wm, ball_pos, tp))
                continue;

            double score = 0.0;
            score += opp_dist * 3.0;
            score += tp.x * 1.0;
            score -= std::fabs(pass_dist - 7.0) * 0.4;

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

    Vector2D getGoalieSupportPoint(const WorldModel &wm)
    {
        const Vector2D ball = wm.ball().pos();

        double x = ball.x - 10.0;
        double y = ball.y * 0.45;

        x = min_max(-46.0, x, -32.0);
        y = min_max(-12.0, y, 12.0);

        if (x > ball.x - 4.0)
        {
            x = ball.x - 4.0;
        }

        return Vector2D(x, y);
    }

    bool selectKeeperReset(const WorldModel &wm,
                           Vector2D *target)
    {
        const Vector2D ball_pos = wm.ball().pos();

        if (ball_pos.x > -5.0)
            return false;

        const PlayerObject *goalie = nullptr;

        for (const PlayerObject *tm : wm.teammatesFromBall())
        {
            if (!tm)
                continue;
            if (!tm->goalie())
                continue;
            if (tm->posCount() > 8)
                continue;

            goalie = tm;
            break;
        }

        if (!goalie)
            return false;

        Vector2D tp = getGoalieSupportPoint(wm);

        if (goalie->pos().dist(tp) > 6.0)
            return false;

        if (!isInsidePitch(tp))
            return false;
        if (ball_pos.dist(tp) > 24.0)
            return false;
        if (nearestOpponentDistance(wm, tp) < 5.0)
            return false;
        if (passLaneBlocked(wm, ball_pos, tp))
            return false;

        if (target)
            *target = tp;
        return true;
    }

    bool isCornerDeliveryBlocked(const WorldModel &wm)
    {
        if (!isCornerKickSituation(wm))
            return false;

        const Vector2D ball_pos = wm.ball().pos();
        Vector2D box_target(ServerParam::i().pitchHalfLength() - 8.0, 0.0);

        for (const PlayerObject *opp : wm.opponentsFromBall())
        {
            if (!opp)
                continue;
            if (opp->posCount() > 8)
                continue;

            const Vector2D opp_pos = opp->pos();

            if (opp_pos.dist(ball_pos) < 6.0)
            {
                return true;
            }

            const double line_dist = distToSegment(opp_pos, ball_pos, box_target);
            if (line_dist < 2.5 && opp_pos.dist(ball_pos) < 12.0)
            {
                return true;
            }
        }

        return false;
    }

    const PlayerObject *selectCornerDiagonalReceiver(const WorldModel &wm,
                                                     Vector2D *target)
    {
        const Vector2D ball_pos = wm.ball().pos();
        const double side_sign = (ball_pos.y >= 0.0 ? 1.0 : -1.0);

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

            if (side_sign * tp.y < 0.0)
                continue;
            if (tp.x < ServerParam::i().pitchHalfLength() - 18.0)
                continue;
            if (tp.x > ServerParam::i().pitchHalfLength() - 4.0)
                continue;

            const double y_from_line = ServerParam::i().pitchHalfWidth() - std::fabs(tp.y);
            if (y_from_line < 4.0 || y_from_line > 16.0)
                continue;

            const double pass_dist = ball_pos.dist(tp);
            if (pass_dist < 5.0 || pass_dist > 16.0)
                continue;

            const double opp_dist = nearestOpponentDistance(wm, tp);
            if (opp_dist < 3.0)
                continue;

            if (passLaneBlocked(wm, ball_pos, tp))
                continue;

            double score = 0.0;
            score += tp.x * 2.0;
            score += opp_dist * 3.0;
            score -= std::fabs(pass_dist - 9.0) * 0.7;
            score -= std::fabs(y_from_line - 8.0) * 0.8;

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

    Vector2D getCornerDiagonalPoint(const WorldModel &wm)
    {
        const double x = ServerParam::i().pitchHalfLength() - 10.0;
        const double y = (ServerParam::i().pitchHalfWidth() - 7.0) * (wm.ball().pos().y > 0.0 ? 1.0 : -1.0);
        return Vector2D(x, y);
    }

    bool shouldBeCornerDiagonalReceiver(const WorldModel &wm)
    {
        if (!isCornerKickSituation(wm))
            return false;
        if (wm.self().goalie())
            return false;
        if (wm.self().distFromBall() < 2.5)
            return false;

        const int unum = wm.self().unum();
        return (unum == 8 || unum == 9 || unum == 11);
    }

    double scoreKickInReceiver(const WorldModel &wm,
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
        score -= std::fabs(pass_dist - 8.0) * 0.5;

        if (dx > 0.0)
        {
            score += std::min(dx, 8.0) * 0.8;
        }

        score -= std::fabs(target.y - ball_pos.y) * 0.08;

        return score;
    }

    const PlayerObject *selectBestKickInReceiver(const WorldModel &wm,
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
            if (tm->posCount() > 8)
                continue;

            Vector2D tp = tm->inertiaFinalPoint();

            const double score = scoreKickInReceiver(wm,
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
        {
            return nullptr;
        }

        if (target)
            *target = best_point;
        return best;
    }

    bool hasSafeKickInOption(const WorldModel &wm)
    {
        Vector2D dummy;

        if (selectBestKickInReceiver(wm, &dummy, true, 14.0, 34.0))
        {
            return true;
        }

        if (selectBestKickInReceiver(wm, &dummy, false, 16.0, 32.0))
        {
            return true;
        }

        if (selectKeeperReset(wm, &dummy))
        {
            return true;
        }

        return false;
    }

}

bool Bhv_SetPlayKickIn::execute(PlayerAgent *agent)
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

void Bhv_SetPlayKickIn::doKick(PlayerAgent *agent)
{
    const WorldModel &wm = agent->world();

    AngleDeg ball_place_angle = (wm.ball().pos().y > 0.0
                                     ? -90.0
                                     : 90.0);
    if (Bhv_GoToPlacedBall(ball_place_angle).execute(agent))
    {
        return;
    }

    if (doKickWait(agent))
    {
        return;
    }

    if (Bhv_PlannedAction().execute(agent))
    {
        agent->setIntention(new IntentionWaitAfterSetPlayKick());
        agent->debugClient().addMessage("KickIn:Plan");
        return;
    }

    if (isCornerKickSituation(wm))
    {
        Vector2D target_point;

        // Use the diagonal receiver when the usual corner lane is crowded early.
        if (isCornerDeliveryBlocked(wm))
        {
            if (selectCornerDiagonalReceiver(wm, &target_point))
            {
                doSmartKick(agent, target_point, 2, "Corner:Diagonal");
                return;
            }
        }

        if (selectCornerAttackTarget(wm, &target_point))
        {
            doSmartKick(agent, target_point, 3, "Corner:Attack");
            return;
        }

        if (selectShortCornerSupport(wm, &target_point))
        {
            doSmartKick(agent, target_point, 2, "Corner:Short");
            return;
        }

        Vector2D danger(ServerParam::i().pitchHalfLength() - 8.0, 0.0);
        doSmartKick(agent, danger, 3, "Corner:ForceBox");
        return;
    }

    Vector2D target_point;

    if (selectBestKickInReceiver(wm, &target_point, true, 14.0, 34.0))
    {
        doSmartKick(agent, target_point, 2, "KickIn:BestForward");
        return;
    }

    if (selectBestKickInReceiver(wm, &target_point, false, 16.0, 32.0))
    {
        doSmartKick(agent, target_point, 2, "KickIn:BestRecycle");
        return;
    }

    if (selectKeeperReset(wm, &target_point))
    {
        doSmartKick(agent, target_point, 3, "KickIn:KeeperReset");
        return;
    }

    if ((wm.ball().angleFromSelf() - wm.self().body()).abs() > 1.5)
    {
        agent->debugClient().addMessage("KickIn:Advance:TurnToBall");
        Body_TurnToBall().execute(agent);
        agent->setNeckAction(new Neck_ScanField());
        return;
    }

    if (wm.self().pos().x < 20.0)
    {
        agent->debugClient().addMessage("KickIn:Advance");
        Body_AdvanceBall().execute(agent);
        agent->setNeckAction(new Neck_ScanField());
        return;
    }

    agent->debugClient().addMessage("KickIn:ForceAdvance");

    Vector2D force_target(ServerParam::i().pitchHalfLength() - 2.0,
                          (ServerParam::i().pitchHalfWidth() - 5.0) * (1.0 - (wm.self().pos().x / ServerParam::i().pitchHalfLength())));
    if (wm.self().pos().y < 0.0)
    {
        force_target.y *= -1.0;
    }

    Body_KickOneStep(force_target,
                     ServerParam::i().ballSpeedMax())
        .execute(agent);
    agent->setNeckAction(new Neck_ScanField());
}

bool Bhv_SetPlayKickIn::doKickWait(PlayerAgent *agent)
{
    const WorldModel &wm = agent->world();

    const int real_set_play_count = static_cast<int>(wm.time().cycle() - wm.lastSetPlayStartTime().cycle());

    if (real_set_play_count >= ServerParam::i().dropBallTime() - 5)
    {
        return false;
    }

    if (Bhv_SetPlay::is_delaying_tactics_situation(agent))
    {
        agent->debugClient().addMessage("KickIn:Delaying");
        Body_TurnToPoint(Vector2D(0.0, 0.0)).execute(agent);
        agent->setNeckAction(new Neck_ScanField());
        return true;
    }

    if (wm.teammatesFromBall().empty())
    {
        agent->debugClient().addMessage("KickIn:NoTeammate");
        Body_TurnToPoint(Vector2D(0.0, 0.0)).execute(agent);
        agent->setNeckAction(new Neck_ScanField());
        return true;
    }

    if (wm.getSetPlayCount() <= 3)
    {
        agent->debugClient().addMessage("KickIn:Wait%d", wm.getSetPlayCount());
        Body_TurnToBall().execute(agent);
        agent->setNeckAction(new Neck_ScanField());
        return true;
    }

    if (wm.getSetPlayCount() <= 12 && !hasSafeKickInOption(wm))
    {
        agent->debugClient().addMessage("KickIn:WaitSafe");
        Body_TurnToBall().execute(agent);
        agent->setNeckAction(new Neck_ScanField());
        return true;
    }

    if (wm.getSetPlayCount() >= 15 && wm.seeTime() == wm.time() && wm.self().stamina() > ServerParam::i().staminaMax() * 0.6)
    {
        return false;
    }

    if (wm.seeTime() != wm.time() || wm.self().stamina() < ServerParam::i().staminaMax() * 0.9)
    {
        Body_TurnToBall().execute(agent);
        agent->setNeckAction(new Neck_ScanField());

        agent->debugClient().addMessage("KickIn:Wait%d", wm.getSetPlayCount());
        return true;
    }

    return false;
}

void Bhv_SetPlayKickIn::doMove(PlayerAgent *agent)
{
    const WorldModel &wm = agent->world();

    Vector2D target_point = Strategy::i().getPosition(wm.self().unum());

    if (isCornerKickSituation(wm))
    {
        const int my_unum = wm.self().unum();
        const bool right_corner = (wm.ball().pos().y < 0.0);
        const bool left_corner = (wm.ball().pos().y > 0.0);

        if ((left_corner && my_unum == 7) || (right_corner && my_unum == 8))
        {
            target_point = getCornerDiagonalPoint(wm);
        }
    }

    bool avoid_opponent = false;
    if (wm.self().stamina() > ServerParam::i().staminaMax() * 0.9)
    {
        const PlayerObject *nearest_opp = wm.getOpponentNearestToSelf(5);

        if (nearest_opp && nearest_opp->pos().dist(target_point) < 3.0)
        {
            Vector2D add_vec = (wm.ball().pos() - target_point);
            add_vec.setLength(3.0);

            long time_val = wm.time().cycle() % 60;
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
            avoid_opponent = true;
        }
    }

    double dash_power = Bhv_SetPlay::get_set_play_dash_power(agent);
    double dist_thr = wm.ball().distFromSelf() * 0.07;
    if (dist_thr < 1.0)
        dist_thr = 1.0;

    agent->debugClient().addMessage("KickInMove");
    agent->debugClient().setTarget(target_point);

    double kicker_ball_dist = (!wm.teammatesFromBall().empty()
                                   ? wm.teammatesFromBall().front()->distFromBall()
                                   : 1000.0);

    if (!Body_GoToPoint(target_point,
                        dist_thr,
                        dash_power)
             .execute(agent))
    {
        if (kicker_ball_dist > 1.0)
        {
            agent->doTurn(120.0);
        }
        else
        {
            Body_TurnToBall().execute(agent);
        }
    }

    Vector2D my_inertia = wm.self().inertiaFinalPoint();
    double wait_dist_buf = (avoid_opponent
                                ? 10.0
                                : wm.ball().pos().dist(target_point) * 0.2 + 6.0);

    if (my_inertia.dist(target_point) > wait_dist_buf || wm.self().stamina() < rcsc::ServerParam::i().staminaMax() * 0.7)
    {
        if (!wm.self().staminaModel().capacityIsEmpty())
        {
            agent->debugClient().addMessage("Sayw");
            agent->addSayMessage(new WaitRequestMessage());
        }
    }

    if (kicker_ball_dist > 3.0)
    {
        agent->setViewAction(new View_Wide());
        agent->setNeckAction(new Neck_ScanField());
    }
    else if (wm.ball().distFromSelf() > 10.0 || kicker_ball_dist > 1.0)
    {
        agent->setNeckAction(new Neck_TurnToBallOrScan(0));
    }
    else
    {
        agent->setNeckAction(new Neck_TurnToBall());
    }
}
