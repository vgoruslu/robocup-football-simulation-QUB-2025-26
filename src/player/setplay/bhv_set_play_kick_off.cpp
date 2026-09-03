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

#include "bhv_set_play_kick_off.h"

#include "strategy.h"

#include "bhv_go_to_placed_ball.h"
#include "bhv_set_play.h"
#include "bhv_prepare_set_play_kick.h"

#include "basic_actions/body_smart_kick.h"

#include "basic_actions/basic_actions.h"
#include "basic_actions/body_go_to_point.h"
// #include "basic_actions/body_kick_one_step.h"
#include "basic_actions/neck_scan_field.h"

#include <rcsc/player/player_agent.h>
#include <rcsc/common/server_param.h>
#include <rcsc/math_util.h>

using namespace rcsc;

/*-------------------------------------------------------------------*/
/*!

*/
bool Bhv_SetPlayKickOff::execute(PlayerAgent *agent)
{
    const WorldModel &wm = agent->world();

    // Helios selected the kickoff taker based on who was closest to the ball.
    // Changed to a fixed role so player 11 consistently starts the routine,
    // making the kickoff more predictable and tactically structured.
    if (wm.self().unum() == 11)
    {
        doKick(agent);
    }
    else
    {
        doMove(agent);
    }

    return true;
}

/*-------------------------------------------------------------------*/
/*!

*/
void Bhv_SetPlayKickOff::doKick(PlayerAgent *agent)
{
    //
    // go to the ball position
    if (Bhv_GoToPlacedBall(0.0).execute(agent))
    {
        return;
    }

    //
    // wait
    //

    if (doKickWait(agent))
    {
        return;
    }

    //
    // kick
    //

    const WorldModel &wm = agent->world();
    const double max_ball_speed = ServerParam::i().maxPower() * wm.self().kickRate();

    // Set a safe default fallback target in our own half.
    // Helios used a more generic target selection path, whereas this supports
    // a controlled reset if the preferred centre-back receiver is unavailable.
    Vector2D target_point(-15.0, 0.0);
    double ball_speed = max_ball_speed;

    // Explicitly track the two centre-backs as preferred kickoff receivers.
    // Unlike Helios, which treated teammates more generically, this introduces
    // role-aware receiver selection for buildup play.
    const PlayerObject *cb2 = nullptr;
    const PlayerObject *cb3 = nullptr;

    // Identify centre-backs by uniform number so kickoff can be routed through
    // defensive buildup players rather than simply using the nearest teammate.
    for (const PlayerObject *tm : wm.teammatesFromSelf())
    {
        if (!tm)
            continue;

        if (tm->unum() == 2)
        {
            cb2 = tm;
        }
        else if (tm->unum() == 3)
        {
            cb3 = tm;
        }
    }

    // Store the final selected receiver after evaluating the available
    // centre-back options.
    const PlayerObject *receiver = nullptr;

    // If both centre-backs are available, choose the easier short option.
    // This keeps the tactical idea of building from the back while still
    // using a practical distance-based choice between the two defenders.
    if (cb2 && cb3)
    {
        receiver = (cb2->distFromSelf() < cb3->distFromSelf()) ? cb2 : cb3;
    }
    else if (cb2)
    {
        receiver = cb2;
    }
    else if (cb3)
    {
        receiver = cb3;
    }

    // Use the selected centre-back as the primary kickoff receiver.
    // This replaces the original nearest-teammate behaviour with a more
    // realistic back-pass restart pattern.
    if (receiver)
    {
        target_point = receiver->inertiaFinalPoint();

        const double dist = wm.ball().pos().dist(target_point);

        // Calculate a suitable first-kick speed so the ball reaches the chosen
        // receiver under server ball-decay constraints, capped by the legal
        // maximum kick speed.
        ball_speed = std::min(max_ball_speed,
                              calc_first_term_geom_series_last(
                                  1.8,
                                  dist,
                                  ServerParam::i().ballDecay()));

        agent->debugClient().addMessage("KickOff:CBReset");
    }
    else
    {
        // Fallback target if neither centre-back is currently available.
        // This preserves a safer reset option instead of forcing a direct
        // forward kickoff.
        target_point.assign(-15.0, 0.0);
    }

    Vector2D ball_vel = Vector2D::polar2vector(ball_speed,
                                               (target_point - wm.ball().pos()).th());
    Vector2D ball_next = wm.ball().pos() + ball_vel;
    while (wm.self().pos().dist(ball_next) < wm.self().playerType().kickableArea() + 0.2)
    {
        ball_vel.setLength(ball_speed + 0.1);
        ball_speed += 0.1;
        ball_next = wm.ball().pos() + ball_vel;
    }

    ball_speed = std::min(ball_speed, max_ball_speed);

    agent->debugClient().setTarget(target_point);

    // enforce one step kick
    Body_SmartKick(target_point,
                   ball_speed,
                   ball_speed * 0.96,
                   1)
        .execute(agent);
    agent->setNeckAction(new Neck_ScanField());
}

/*-------------------------------------------------------------------*/
/*!

*/
bool Bhv_SetPlayKickOff::doKickWait(PlayerAgent *agent)
{
    const WorldModel &wm = agent->world();

    const int real_set_play_count = static_cast<int>(wm.time().cycle() - wm.lastSetPlayStartTime().cycle());

    if (real_set_play_count >= ServerParam::i().dropBallTime() - 5)
    {
        return false;
    }

    if (Bhv_SetPlay::is_delaying_tactics_situation(agent))
    {
        agent->debugClient().addMessage("KickOff:Delaying");

        Body_TurnToAngle(180.0).execute(agent);
        agent->setNeckAction(new Neck_ScanField());
        return true;
    }

    if (wm.self().body().abs() < 175.0)
    {
        agent->debugClient().addMessage("KickOff:Turn");

        Body_TurnToAngle(180.0).execute(agent);
        agent->setNeckAction(new Neck_ScanField());
        return true;
    }

    if (wm.teammatesFromBall().empty())
    {
        agent->debugClient().addMessage("KickOff:NoTeammate");

        Body_TurnToAngle(180.0).execute(agent);
        agent->setNeckAction(new Neck_ScanField());
        return true;
    }

    if (wm.teammatesFromSelf().size() < 9)
    {
        agent->debugClient().addMessage("FreeKick:Wait%d", real_set_play_count);

        Body_TurnToAngle(180.0).execute(agent);
        agent->setNeckAction(new Neck_ScanField());
        return true;
    }

    // Add a short tactical delay before kickoff so teammates have time to
    // spread into shape. Helios mainly waited for technical readiness;
    // this adds a simple form of tactical readiness as well.
    if (wm.getSetPlayCount() <= 6)
    {
        agent->debugClient().addMessage("KickOff:ShapeWait%d", wm.getSetPlayCount());

        Body_TurnToAngle(180.0).execute(agent);
        agent->setNeckAction(new Neck_ScanField());
        return true;
    }

    if (wm.seeTime() != wm.time() || wm.self().stamina() < ServerParam::i().staminaMax() * 0.9)
    {
        agent->debugClient().addMessage("KickOff:WaitX");

        // Body_TurnToBall().execute( agent );
        Body_TurnToAngle(180.0).execute(agent);
        agent->setNeckAction(new Neck_ScanField());
        return true;
    }

    return false;
}

/*-------------------------------------------------------------------*/
/*!

*/
void Bhv_SetPlayKickOff::doMove(PlayerAgent *agent)
{
    const WorldModel &wm = agent->world();

    Vector2D target_point;

    if (wm.self().unum() != 11)
    {
        target_point = Strategy::i().getPosition(wm.self().unum());
        target_point.x = std::min(-0.5, target_point.x);

        // Move player 6 slightly away from the direct centre-back passing lane
        // to reduce congestion and improve spacing during the kickoff routine.
        if (wm.self().unum() == 6)
        {
            target_point.y += 6.0;
        }
    }
    else
    {
        // Ensure the designated kickoff taker moves directly to the ball.
        // This supports the fixed-role kickoff design introduced above.
        target_point = wm.ball().pos(); // kickoff taker goes to ball
    }

    double dash_power = Bhv_SetPlay::get_set_play_dash_power(agent);
    double dist_thr = wm.ball().distFromSelf() * 0.07;
    if (dist_thr < 1.0)
        dist_thr = 1.0;

    if (!Body_GoToPoint(target_point,
                        dist_thr,
                        dash_power)
             .execute(agent))
    {
        Body_TurnToBall().execute(agent);
    }
    agent->setNeckAction(new Neck_ScanField());

    agent->debugClient().setTarget(target_point);
}
