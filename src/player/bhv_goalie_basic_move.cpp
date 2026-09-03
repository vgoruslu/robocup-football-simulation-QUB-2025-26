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

#include "bhv_goalie_basic_move.h"

#include "bhv_basic_tackle.h"
#include "neck_goalie_turn_neck.h"

#include "basic_actions/basic_actions.h"
#include "basic_actions/body_go_to_point.h"
#include "basic_actions/body_stop_dash.h"
#include "basic_actions/bhv_go_to_point_look_ball.h"

#include <rcsc/player/player_agent.h>
#include <rcsc/player/intercept_table.h>
#include <rcsc/player/debug_client.h>

#include <rcsc/common/server_param.h>
#include <rcsc/geom/line_2d.h>
#include <rcsc/soccer_math.h>

using namespace rcsc;

// Anonymous namespace:
// Keeps these helper functions private to this .cpp file only.
// They are implementation details for Bhv_GoalieBasicMove and are
// not intended to be part of the public class interface.
namespace
{

    // Decide whether the goalkeeper should switch into predictive mode.
    bool shouldUsePredictiveGoalie(const rcsc::WorldModel &wm)
    {
        if (wm.gameMode().type() != rcsc::GameMode::PlayOn)
        {
            return false;
        }

        // ball      = current ball position
        // opp_step  = predicted cycles until the nearest opponent reaches the ball
        // mate_step = predicted cycles until the nearest teammate reaches the ball
        const rcsc::Vector2D ball = wm.ball().pos();
        const int opp_step = wm.interceptTable().opponentStep();
        const int mate_step = wm.interceptTable().teammateStep();

        // if an opponent is already kickable, they may shoot immediately,
        // so predictive positioning should activate right away.
        if (wm.kickableOpponent())
        {
            return true;
        }

        // High danger case:
        // The ball is already deep in our half, so the goalkeeper should be
        // more cautious. Predictive mode is triggered if either:
        // - the opponent can reach the ball before, at the same time as,
        //   or within 2 cycles of our teammate
        // - the ball is already moving toward our goal at a meaningful speed
        //
        // Justification:
        // Because the ball is close to our goal, even a small opponent timing
        // threat or a forward-moving ball can quickly become a shooting chance.
        if (ball.x < -24.0 && (opp_step <= mate_step + 2 || wm.ball().vel().x < -0.3))
        {
            return true;
        }

        // Medium danger / early trigger case:
        // The ball is entering a threatening area, but is not as deep yet.
        // Predictive mode is only triggered if the opponent is very close to
        // reaching the ball: before us, at the same time, or within 1 cycle.
        //
        // Justification:
        // This lets the goalkeeper prepare early for a developing attack,
        // while keeping the trigger strict enough to avoid overreacting.
        if (ball.x < -18.0 && opp_step <= mate_step + 1)
        {
            return true;
        }

        // Otherwise the situation is not dangerous enough for predictive mode.
        return false;
    }

    // Compute the goalkeeper's target position in predictive mode.
    // Return a 2D position for the goalkeeper, based on the current game state.
    rcsc::Vector2D getPredictiveGoaliePos(const rcsc::WorldModel &wm)
    {
        // Get field dimensions and the current ball position.
        // ServerParam gives things like pitch size, goal size and goal position
        const rcsc::ServerParam &SP = rcsc::ServerParam::i();
        const rcsc::Vector2D ball = wm.ball().pos();

        // Geometry of our goal:
        // goal_x       = x-coordinate of our goal line
        // left_post_y  = y-coordinate of one goal post
        // right_post_y = y-coordinate of the other goal post
        // ball_width        = how wide the ball is from the center line, ignoring sign

        // x-position of our goal line (back of the pitch)
        const double goal_x = -SP.pitchHalfLength();
        // y-positions of the goal posts (top and bottom of goal)
        const double left_post_y = -SP.goalHalfWidth();
        const double right_post_y = SP.goalHalfWidth();
        // how far the ball is from the centre line (ignores left/right side) fabss means absoulte value remove the sign +/-
        const double ball_width = std::fabs(ball.y);

        // This will represent the y-coordinate inside the goal
        // that we think the attacker is most likely to aim at.
        double target_y = 0.0;

        // If the ball is very wide, bias the likely shot target toward the near post.
        if (ball_width > 10.0)
        {
            target_y = (ball.y > 0.0
                            ? right_post_y - 0.25
                            : left_post_y + 0.25);
        }
        // Semi-central case:
        // The ball is somewhat wide, so slightly bias the predicted shot
        // toward the same side as the ball, but keep the goalkeeper mostly central.
        else if (ball_width > 5.0)
        {
            target_y = ball.y * 0.35;
        }
        // Central case:
        // The ball is near the middle, so keep the predicted shot target
        // close to the centre of the goal to maintain balanced coverage.
        else
        {
            target_y = ball.y * 0.15;
        }

        // Predicted shot target inside our goal (x = goal line, y = estimated shot position)
        rcsc::Vector2D shot_target(goal_x, target_y);

        // Build a vector from the guessed shot target to the ball.
        // This acts as an approximation of the likely shot line.
        rcsc::Vector2D shot_line = ball - shot_target;

        // Compute the length of that vector.
        const double len = shot_line.r();

        // Safety fallback:
        // If the value is extremely small, it doesn’t represent meaningful movement in the game,
        // and using it could cause unstable calculations when we divide by it, so we treat it as zero and use a safe fallback.
        if (len < 0.001)
        {
            return rcsc::Vector2D(goal_x + 0.8, 0.0);
        }

        // Normalize the shot line so it has length 1.
        // This makes it easy to move a chosen distance along the line.
        shot_line = shot_line / len;

        // depth = how far in front of the goal the keeper should stand
        // along the estimated shot line.
        // Start with a conservative default.
        double depth = 1.0;

        // Very close and central danger:
        // come farther off the line to reduce the shooting angle more aggressively.
        if (ball.x < -44.0 && ball_width < 8.0)
        {
            depth = 2.7;
        }
        // Close and somewhat central:
        // still aggressive, but slightly less so.
        else if (ball.x < -40.0 && ball_width < 12.0)
        {
            depth = 2.2;
        }
        // Close but wide:
        // stay deeper because the shooting angle is already narrow.
        else if (ball.x < -42.0 && ball_width >= 12.0)
        {
            depth = 1.2;
        }
        // Moderately deep danger:
        // use a medium proactive step.
        else if (ball.x < -32.0)
        {
            depth = 1.5;
        }

        // Final predictive target:
        // start from the guessed shot target, then move along the shot line
        // by the chosen depth.
        rcsc::Vector2D target = shot_target + shot_line * depth;

        // Allowed x-range for the keeper in predictive mode.
        // This stops the keeper from staying on the line or coming too far out.
        double min_x = goal_x + 0.7;
        double max_x = goal_x + 2.8;

        // On very wide angles, reduce how far out the keeper can come.
        if (ball_width > 12.0)
        {
            max_x = goal_x + 1.6;
        }

        // Clamp final x-position to stay within the allowed predictive depth range.
        target.x = rcsc::bound(min_x, target.x, max_x);

        // Clamp final y-position so the keeper stays inside the goal mouth
        // with a small margin from the posts.
        target.y = rcsc::bound(left_post_y + 0.2,
                               target.y,
                               right_post_y - 0.2);

        // Return the final predictive goalkeeper target.
        return target;
    }

    // Classify whether the current state is dangerous enough that the keeper
    // should remain conservative and avoid aggressive support behaviour.
    //
    // This helper is mainly used to stop the sweeper/support logic from activating
    // when the risk of conceding is too high.
    bool isGoalieDangerState(const rcsc::WorldModel &wm)
    {
        // Store key game state info for danger checks
        const rcsc::Vector2D ball = wm.ball().pos();
        const rcsc::Vector2D vel = wm.ball().vel();
        const rcsc::Vector2D self = wm.self().pos();

        const int tm_step = wm.interceptTable().teammateStep();
        const int opp_step = wm.interceptTable().opponentStep();

        // If the opponent can already kick the ball, this is immediately dangerous.
        if (wm.kickableOpponent())
        {
            return true;
        }

        // If the ball is deep in our half, treat it as dangerous.
        if (ball.x < -30.0)
        {
            return true;
        }

        // If the ball is in our half and the opponent reaches it before our team,

        if (ball.x < -12.0 && opp_step < tm_step)
        {
            return true;
        }

        // If the ball is fairly central in our half and moving toward our goal,
        // classify that as danger.
        if (ball.x < -15.0 && std::fabs(ball.y) < 18.0 && vel.x < -0.5)
        {
            return true;
        }

        // If the keeper is already somewhat advanced, and the ball is in a risky area,
        // and the opponent reaches first, the state is dangerous.
        if (self.x > -38.0 && ball.x < -20.0 && opp_step < tm_step)
        {
            return true;
        }

        // Otherwise this is not considered a danger state.
        return false;
    }

    // Decide whether the goalkeeper can safely support play like a sweeper keeper.
    //
    // Support mode is intended for safer situations where our team likely controls
    // the ball and the keeper can step up as a passing option without exposing goal.
    bool shouldGoalieSupport(const rcsc::WorldModel &wm)
    {
        // Cache the current game mode.
        const rcsc::GameMode::Type mode = wm.gameMode().type();

        // Only allow support during:
        // - normal open play
        // - kick-ins
        if (!(mode == rcsc::GameMode::PlayOn || mode == rcsc::GameMode::KickIn_))
        {
            return false;
        }

        // If it is a kick-in, only support on our own kick-in.
        // Never step up when the opponent has the restart.
        if (mode == rcsc::GameMode::KickIn_ && wm.gameMode().side() != wm.ourSide())
        {
            return false;
        }

        // If the situation is dangerous, support mode must be disabled.
        if (isGoalieDangerState(wm))
        {
            return false;
        }

        // Cache state needed for support decisions.
        const rcsc::Vector2D ball = wm.ball().pos();
        const rcsc::Vector2D self = wm.self().pos();
        const int tm_step = wm.interceptTable().teammateStep();
        const int opp_step = wm.interceptTable().opponentStep();

        // Rough possession estimate:
        // true if a teammate already controls the ball, or our team is expected
        // to reach it at least as quickly as the opponent.
        const bool our_possession =
            wm.kickableTeammate() || (tm_step <= opp_step + 1);

        // During live play, do not support unless we likely have possession.
        if (mode == rcsc::GameMode::PlayOn && !our_possession)
        {
            return false;
        }

        // If the ball is deep and central in our half, stay conservative.
        if (ball.x < -30.0 && std::fabs(ball.y) < 18.0)
        {
            return false;
        }

        // If the keeper is already too far forward, do not advance further.
        if (self.x > -35.0)
        {
            return false;
        }

        // If all checks passed, support mode is allowed.
        return true;
    }

    // Compute the goalkeeper’s target position when acting as a support player
    // (i.e., sweeper-keeper behaviour).
    //
    // The idea is:
    // - stay behind the ball (never ahead of play)
    // - provide a safe passing option
    // - remain close enough to goal to recover if needed
    rcsc::Vector2D getGoalieSupportPoint(const rcsc::WorldModel &wm)
    {
        // Get current ball position
        const rcsc::Vector2D ball = wm.ball().pos();

        // Get current game mode (used to slightly adjust behaviour)
        const rcsc::GameMode::Type mode = wm.gameMode().type();

        // offset = how far behind the ball (in x direction) the keeper stays
        // y_scale = how much the keeper follows the ball laterally (side-to-side)
        double offset = 8.0;
        double y_scale = 0.20;

        // If this is a kick-in situation:
        // - stay slightly deeper (more conservative)
        // - follow the ball less in the y direction
        if (mode == rcsc::GameMode::KickIn_)
        {
            offset = 9.0;
            y_scale = 0.15;
        }
        else
        {
            // During normal play, adjust how aggressive the support position is
            // based on how far up the field the ball is.

            // Ball is far upfield → keeper can step higher safely
            if (ball.x > -10.0)
                offset = 11.0;

            // Ball is mid-field → moderate support distance
            else if (ball.x > -20.0)
                offset = 9.0;

            // Ball is deeper → stay closer to goal (safer)
            else
                offset = 7.0;
        }

        // Compute raw support position:

        // X: stay behind the ball by the chosen offset
        double x = ball.x - offset;

        // Y: partially follow the ball sideways (scaled down)
        // This keeps the keeper roughly central while still supporting shape
        double y = ball.y * y_scale;

        // Clamp X position:
        // - not too deep (don’t sit inside goal unnecessarily)
        // - not too far forward (don’t expose goal)
        x = rcsc::bound(-48.5, x, -35.0);

        // Clamp Y position:
        // keep the keeper reasonably central and not drifting too wide
        y = rcsc::bound(-10.0, y, 10.0);

        // Return the final support position
        return rcsc::Vector2D(x, y);
    }

}
/*-------------------------------------------------------------------*/
/*!

 */
bool Bhv_GoalieBasicMove::execute(PlayerAgent *agent)
{
    // Get a const reference to the current world model.
    // This gives access to the goalkeeper’s latest view of the game state
    // without copying the WorldModel object.
    const WorldModel &wm = agent->world();

    // Compute the original/default HELIOS goalie target point.
    // This is the fallback position the keeper would normally use
    // if none of the newly added support or predictive behaviours activate.
    const Vector2D move_point = getTargetPoint(agent);

    // ------------------------------------------------------------------
    // 1. Sweeper-keeper support mode
    // ------------------------------------------------------------------
    // First, check whether it is safe for the goalkeeper to step up
    // and act as a support player.
    //
    // This mode should only activate when:
    // - the game state is safe
    // - our team likely has possession
    // - the keeper is not already too advanced
    if (shouldGoalieSupport(wm))
    {
        // Compute the support position for the goalkeeper.
        // This places the keeper behind the ball as a safe outlet option,
        // while keeping them close enough to recover defensively.
        const Vector2D support_point = getGoalieSupportPoint(wm);

        // Try to move the goalkeeper to that support position.
        //
        // Body_GoToPoint arguments:
        // 1) support_point = target position
        // 2) 0.8           = distance threshold / acceptable closeness
        // 3) maxDashPower * 0.7 = moderate dash power
        //
        // Using 70% of max dash power makes support movement controlled
        // rather than overly aggressive.
        if (Body_GoToPoint(support_point,
                           0.8,
                           ServerParam::i().maxDashPower() * 0.7)
                .execute(agent))
        {
            // Add a debug message so the current behaviour can be seen
            // in the monitor/debug client during testing.
            agent->debugClient().addMessage("SweeperKeeper");

            // Mark the chosen target point in the debug client.
            agent->debugClient().setTarget(support_point);

            // Keep the goalkeeper’s neck focused on the ball
            // while the body moves into the support position.
            agent->setNeckAction(new Neck_TurnToBall());

            // Return immediately because an action has already been chosen
            // for this cycle.
            return true;
        }
    }

    // ------------------------------------------------------------------
    // 2. Predictive goalkeeper mode
    // ------------------------------------------------------------------
    // If support mode did not activate, check whether the current situation
    // is dangerous enough to justify proactive predictive positioning.
    //
    // This mode is intended to move the keeper onto an estimated shot line
    // before the threat becomes immediate.
    if (shouldUsePredictiveGoalie(wm))
    {
        // Compute the predictive target point using the helper function.
        // This position is based on estimated shot target, angle, and depth.
        const Vector2D target = getPredictiveGoaliePos(wm);

        // Move aggressively to the predictive target.
        //
        // Body_GoToPoint arguments:
        // 1) target = predictive goalkeeper position
        // 2) 0.1    = very tight distance threshold
        // 3) maxDashPower = full urgency / full dash power
        //
        // This is more aggressive than support mode because predictive mode
        // is used during danger rather than safe possession.
        if (Body_GoToPoint(target,
                           0.1,
                           ServerParam::i().maxDashPower())
                .execute(agent))
        {
            // Debug label for visualising when predictive mode activates.
            agent->debugClient().addMessage("PredictiveGoalie");

            // Show the predictive target in the debug client.
            agent->debugClient().setTarget(target);

            // Keep the keeper visually locked on the ball while moving.
            agent->setNeckAction(new Neck_TurnToBall());

            // Return immediately because the goalkeeper action for this cycle
            // has already been determined.
            return true;
        }
    }

    //////////////////////////////////////////////////////////////////
    // tackle
    if (Bhv_BasicTackle(0.8, 90.0).execute(agent))
    {
        return true;
    }

    /////////////////////////////////////////////////////////////
    //----------------------------------------------------------
    if (doPrepareDeepCross(agent, move_point))
    {
        // face to opponent side to wait the opponent last cross pass
        return true;
    }
    //----------------------------------------------------------
    // check distance to the move target point
    // if already there, try to stop
    if (doStopAtMovePoint(agent, move_point))
    {
        // execute stop action
        return true;
    }
    //----------------------------------------------------------
    // check whether ball is in very dangerous state
    if (doMoveForDangerousState(agent, move_point))
    {
        // execute emergency action
        return true;
    }
    //----------------------------------------------------------
    // check & correct X difference
    if (doCorrectX(agent, move_point))
    {
        // execute x-pos adjustment action
        return true;
    }

    //----------------------------------------------------------
    if (doCorrectBodyDir(agent, move_point, true)) // consider opp
    {
        // exeucte turn
        return true;
    }

    //----------------------------------------------------------
    if (doGoToMovePoint(agent, move_point))
    {
        // mainly execute Y-adjustment if body direction is OK. -> only dash
        // if body direction is not good, nomal go to action is done.
        return true;
    }

    //----------------------------------------------------------
    // change my body angle to desired angle
    if (doCorrectBodyDir(agent, move_point, false)) // not consider opp
    {
        return true;
    }

    agent->debugClient().addMessage("OnlyTurnNeck");

    agent->doTurn(0.0);
    agent->setNeckAction(new Neck_GoalieTurnNeck());

    return true;
}

/*-------------------------------------------------------------------*/
/*!

 */
Vector2D
Bhv_GoalieBasicMove::getTargetPoint(PlayerAgent *agent)
{
    const double base_move_x = -49.8;
    const double danger_move_x = -51.5;
    const WorldModel &wm = agent->world();

    int ball_reach_step = 0;
    if (!wm.kickableTeammate() && !wm.kickableOpponent())
    {
        ball_reach_step = std::min(wm.interceptTable().teammateStep(),
                                   wm.interceptTable().opponentStep());
    }
    const Vector2D base_pos = wm.ball().inertiaPoint(ball_reach_step);

    //---------------------------------------------------------//
    // angle is very dangerous
    if (base_pos.y > ServerParam::i().goalHalfWidth() + 3.0)
    {
        Vector2D right_pole(-ServerParam::i().pitchHalfLength(),
                            ServerParam::i().goalHalfWidth());
        AngleDeg angle_to_pole = (right_pole - base_pos).th();

        if (-140.0 < angle_to_pole.degree() && angle_to_pole.degree() < -90.0)
        {
            agent->debugClient().addMessage("RPole");
            return Vector2D(danger_move_x, ServerParam::i().goalHalfWidth() + 0.001);
        }
    }
    else if (base_pos.y < -ServerParam::i().goalHalfWidth() - 3.0)
    {
        Vector2D left_pole(-ServerParam::i().pitchHalfLength(),
                           -ServerParam::i().goalHalfWidth());
        AngleDeg angle_to_pole = (left_pole - base_pos).th();

        if (90.0 < angle_to_pole.degree() && angle_to_pole.degree() < 140.0)
        {
            agent->debugClient().addMessage("LPole");
            return Vector2D(danger_move_x, -ServerParam::i().goalHalfWidth() - 0.001);
        }
    }

    //---------------------------------------------------------//
    // ball is close to goal line
    if (base_pos.x < -ServerParam::i().pitchHalfLength() + 8.0 && base_pos.absY() > ServerParam::i().goalHalfWidth() + 2.0)
    {
        Vector2D target_point(base_move_x, ServerParam::i().goalHalfWidth() - 0.1);
        if (base_pos.y < 0.0)
        {
            target_point.y *= -1.0;
        }

        agent->debugClient().addMessage("Pos(1)");

        return target_point;
    }

    //---------------------------------------------------------//
    {
        const double x_back = 7.0; // tune this!!
        int ball_pred_cycle = 5;   // tune this!!
        const double y_buf = 0.5;  // tune this!!
        const Vector2D base_point(-ServerParam::i().pitchHalfLength() - x_back,
                                  0.0);
        Vector2D ball_point;
        if (wm.kickableOpponent())
        {
            ball_point = base_pos;
            agent->debugClient().addMessage("Pos(2)");
        }
        else
        {
            int opp_min = wm.interceptTable().opponentStep();
            if (opp_min < ball_pred_cycle)
            {
                ball_pred_cycle = opp_min;
            }

            ball_point = inertia_n_step_point(base_pos,
                                              wm.ball().vel(),
                                              ball_pred_cycle,
                                              ServerParam::i().ballDecay());
            agent->debugClient().addMessage("Pos(3)");
        }

        if (ball_point.x < base_point.x + 0.1)
        {
            ball_point.x = base_point.x + 0.1;
        }

        Line2D ball_line(ball_point, base_point);
        double move_y = ball_line.getY(base_move_x);

        if (move_y > ServerParam::i().goalHalfWidth() - y_buf)
        {
            move_y = ServerParam::i().goalHalfWidth() - y_buf;
        }
        if (move_y < -ServerParam::i().goalHalfWidth() + y_buf)
        {
            move_y = -ServerParam::i().goalHalfWidth() + y_buf;
        }

        return Vector2D(base_move_x, move_y);
    }
}

/*-------------------------------------------------------------------*/
/*!

 */
double
Bhv_GoalieBasicMove::getBasicDashPower(PlayerAgent *agent,
                                       const Vector2D &move_point)
{
    const WorldModel &wm = agent->world();
    const PlayerType &mytype = wm.self().playerType();

    const double my_inc = mytype.staminaIncMax() * wm.self().recovery();

    if (std::fabs(wm.self().pos().x - move_point.x) > 3.0)
    {
        return ServerParam::i().maxDashPower();
    }

    if (wm.ball().pos().x > -30.0)
    {
        if (wm.self().stamina() < ServerParam::i().staminaMax() * 0.9)
        {
            return my_inc * 0.5;
        }
        agent->debugClient().addMessage("P1");
        return my_inc;
    }
    else if (wm.ball().pos().x > ServerParam::i().ourPenaltyAreaLineX())
    {
        if (wm.ball().pos().absY() > 20.0)
        {
            // penalty area
            agent->debugClient().addMessage("P2");
            return my_inc;
        }
        if (wm.ball().vel().x > 1.0)
        {
            // ball is moving to opponent side
            agent->debugClient().addMessage("P2.5");
            return my_inc * 0.5;
        }

        int opp_min = wm.interceptTable().opponentStep();
        if (opp_min <= 3)
        {
            agent->debugClient().addMessage("P2.3");
            return ServerParam::i().maxDashPower();
        }

        if (wm.self().stamina() < ServerParam::i().staminaMax() * 0.7)
        {
            agent->debugClient().addMessage("P2.6");
            return my_inc * 0.7;
        }
        agent->debugClient().addMessage("P3");
        return ServerParam::i().maxDashPower() * 0.6;
    }
    else
    {
        if (wm.ball().pos().absY() < 15.0 || wm.ball().pos().y * wm.self().pos().y < 0.0) // opposite side
        {
            agent->debugClient().addMessage("P4");
            return ServerParam::i().maxDashPower();
        }
        else
        {
            agent->debugClient().addMessage("P5");
            return my_inc;
        }
    }
}

/*-------------------------------------------------------------------*/
/*!

 */
bool Bhv_GoalieBasicMove::doPrepareDeepCross(PlayerAgent *agent,
                                             const Vector2D &move_point)
{
    if (move_point.absY() < ServerParam::i().goalHalfWidth() - 0.8)
    {
        // consider only very deep cross
        return false;
    }

    const WorldModel &wm = agent->world();

    const Vector2D goal_c(-ServerParam::i().pitchHalfLength(), 0.0);

    Vector2D goal_to_ball = wm.ball().pos() - goal_c;

    if (goal_to_ball.th().abs() < 60.0)
    {
        // ball is not in side cross area
        return false;
    }

    Vector2D my_inertia = wm.self().inertiaFinalPoint();
    double dist_thr = wm.ball().distFromSelf() * 0.1;
    if (dist_thr < 0.5)
        dist_thr = 0.5;
    // double dist_thr = 0.5;

    if (my_inertia.dist(move_point) > dist_thr)
    {
        // needed to go to move target point
        double dash_power = getBasicDashPower(agent, move_point);
        agent->debugClient().addMessage("DeepCrossMove%.0f", dash_power);
        agent->debugClient().setTarget(move_point);
        agent->debugClient().addCircle(move_point, dist_thr);

        doGoToPointLookBall(agent,
                            move_point,
                            wm.ball().angleFromSelf(),
                            dist_thr,
                            dash_power);
        return true;
    }

    AngleDeg body_angle = (wm.ball().pos().y < 0.0
                               ? 10.0
                               : -10.0);
    agent->debugClient().addMessage("PrepareCross");
    agent->debugClient().setTarget(move_point);

    Body_TurnToAngle(body_angle).execute(agent);
    agent->setNeckAction(new Neck_GoalieTurnNeck());
    return true;
}

/*-------------------------------------------------------------------*/
/*!

 */
bool Bhv_GoalieBasicMove::doStopAtMovePoint(PlayerAgent *agent,
                                            const Vector2D &move_point)
{
    //----------------------------------------------------------
    // already exist at target point
    // but inertia movement is big
    // stop dash

    const WorldModel &wm = agent->world();
    double dist_thr = wm.ball().distFromSelf() * 0.1;
    if (dist_thr < 0.5)
        dist_thr = 0.5;

    // now, in the target area
    if (wm.self().pos().dist(move_point) < dist_thr)
    {
        const Vector2D my_final = inertia_final_point(wm.self().pos(),
                                                      wm.self().vel(),
                                                      wm.self().playerType().playerDecay());
        // after inertia move, can stay in the target area
        if (my_final.dist(move_point) < dist_thr)
        {
            agent->debugClient().addMessage("InertiaStay");
            return false;
        }

        // try to stop at the current point
        agent->debugClient().addMessage("Stop");
        agent->debugClient().setTarget(move_point);

        Body_StopDash(true).execute(agent); // save recovery
        agent->setNeckAction(new Neck_GoalieTurnNeck());
        return true;
    }

    return false;
}

/*-------------------------------------------------------------------*/
/*!

 */
bool Bhv_GoalieBasicMove::doMoveForDangerousState(PlayerAgent *agent,
                                                  const Vector2D &move_point)
{
    const WorldModel &wm = agent->world();

    const double x_buf = 0.5;

    const Vector2D ball_next = wm.ball().pos() + wm.ball().vel();

    if (std::fabs(move_point.x - wm.self().pos().x) > x_buf && ball_next.x < -ServerParam::i().pitchHalfLength() + 11.0 && ball_next.absY() < ServerParam::i().goalHalfWidth() + 1.0)
    {
        // x difference to the move point is over threshold
        // but ball is in very dangerous area (just front of our goal)

        // and, exist opponent close to ball
        if (!wm.opponentsFromBall().empty() && wm.opponentsFromBall().front()->distFromBall() < 2.0)
        {
            Vector2D block_point = wm.opponentsFromBall().front()->pos();
            block_point.x -= 2.5;
            block_point.y = move_point.y;

            if (wm.self().pos().x < block_point.x)
            {
                block_point.x = wm.self().pos().x;
            }

            agent->debugClient().addMessage("BlockOpp");

            if (doGoToMovePoint(agent, block_point))
            {
                return true;
            }

            double dist_thr = wm.ball().distFromSelf() * 0.1;
            if (dist_thr < 0.5)
                dist_thr = 0.5;

            agent->debugClient().setTarget(block_point);
            agent->debugClient().addCircle(block_point, dist_thr);

            doGoToPointLookBall(agent,
                                move_point,
                                wm.ball().angleFromSelf(),
                                dist_thr,
                                ServerParam::i().maxDashPower());
            return true;
        }
    }

    return false;
}

/*-------------------------------------------------------------------*/
/*!

 */
bool Bhv_GoalieBasicMove::doCorrectX(PlayerAgent *agent,
                                     const Vector2D &move_point)
{
    const WorldModel &wm = agent->world();

    const double x_buf = 0.5;

    if (std::fabs(move_point.x - wm.self().pos().x) < x_buf)
    {
        // x difference is already small.
        return false;
    }

    int opp_min_cyc = wm.interceptTable().opponentStep();
    if ((!wm.kickableOpponent() && opp_min_cyc >= 4) || wm.ball().distFromSelf() > 18.0)
    {
        double dash_power = getBasicDashPower(agent, move_point);

        agent->debugClient().addMessage("CorrectX%.0f", dash_power);
        agent->debugClient().setTarget(move_point);
        agent->debugClient().addCircle(move_point, x_buf);

        if (!wm.kickableOpponent() && wm.ball().distFromSelf() > 30.0)
        {
            if (!Body_GoToPoint(move_point, x_buf, dash_power).execute(agent))
            {
                AngleDeg body_angle = (wm.self().body().degree() > 0.0
                                           ? 90.0
                                           : -90.0);
                Body_TurnToAngle(body_angle).execute(agent);
            }
            agent->setNeckAction(new Neck_TurnToBall());
            return true;
        }

        doGoToPointLookBall(agent,
                            move_point,
                            wm.ball().angleFromSelf(),
                            x_buf,
                            dash_power);
        return true;
    }

    return false;
}

/*-------------------------------------------------------------------*/
/*!

 */
bool Bhv_GoalieBasicMove::doCorrectBodyDir(PlayerAgent *agent,
                                           const Vector2D &move_point,
                                           const bool consider_opp)
{
    // adjust only body direction

    const WorldModel &wm = agent->world();

    const Vector2D ball_next = wm.ball().pos() + wm.ball().vel();

    const AngleDeg target_angle = (ball_next.y < 0.0 ? -90.0 : 90.0);
    const double angle_diff = (wm.self().body() - target_angle).abs();

    if (angle_diff < 5.0)
    {
        return false;
    }

#if 1
    {
        const Vector2D goal_c(-ServerParam::i().pitchHalfLength(), 0.0);
        Vector2D goal_to_ball = wm.ball().pos() - goal_c;
        if (goal_to_ball.th().abs() >= 60.0)
        {
            return false;
        }
    }
#else
    if (wm.ball().pos().x < -36.0 && wm.ball().pos().absY() < 15.0 && wm.self().pos().dist(move_point) > 1.5)
    {
        return false;
    }
#endif

    double opp_ball_dist = (wm.opponentsFromBall().empty()
                                ? 100.0
                                : wm.opponentsFromBall().front()->distFromBall());
    if (!consider_opp || opp_ball_dist > 7.0 || wm.ball().distFromSelf() > 20.0 || (std::fabs(move_point.y - wm.self().pos().y) < 1.0 // y diff
                                                                                    && !wm.kickableOpponent()))
    {
        agent->debugClient().addMessage("CorrectBody%s",
                                        consider_opp ? "WithOpp" : "");
        Body_TurnToAngle(target_angle).execute(agent);
        agent->setNeckAction(new Neck_GoalieTurnNeck());
        return true;
    }

    return false;
}

/*-------------------------------------------------------------------*/
/*!

 */
bool Bhv_GoalieBasicMove::doGoToMovePoint(PlayerAgent *agent,
                                          const Vector2D &move_point)
{
    // move to target point
    // check Y coordinate difference

    const WorldModel &wm = agent->world();

    double dist_thr = wm.ball().distFromSelf() * 0.08;
    if (dist_thr < 0.5)
        dist_thr = 0.5;

    const double y_diff = std::fabs(move_point.y - wm.self().pos().y);
    if (y_diff < dist_thr)
    {
        // already there
        return false;
    }

    //----------------------------------------------------------//
    // dash to body direction

    double dash_power = getBasicDashPower(agent, move_point);

    // body direction is OK
    if (std::fabs(wm.self().body().abs() - 90.0) < 7.0)
    {
        // calc dash power only to reach the target point
        double required_power = y_diff / wm.self().dashRate();
        if (dash_power > required_power)
        {
            dash_power = required_power;
        }

        if (move_point.y > wm.self().pos().y)
        {
            if (wm.self().body().degree() < 0.0)
            {
                dash_power *= -1.0;
            }
        }
        else
        {
            if (wm.self().body().degree() > 0.0)
            {
                dash_power *= -1.0;
            }
        }

        dash_power = ServerParam::i().normalizeDashPower(dash_power);

        agent->debugClient().addMessage("CorrectY(1)%.0f", dash_power);
        agent->debugClient().setTarget(move_point);

        agent->doDash(dash_power);
        agent->setNeckAction(new Neck_GoalieTurnNeck());
    }
    else
    {
        agent->debugClient().addMessage("CorrectPos%.0f", dash_power);
        agent->debugClient().setTarget(move_point);
        agent->debugClient().addCircle(move_point, dist_thr);

        doGoToPointLookBall(agent,
                            move_point,
                            wm.ball().angleFromSelf(),
                            dist_thr,
                            dash_power);
    }
    return true;
}

/*-------------------------------------------------------------------*/
/*!

 */
void Bhv_GoalieBasicMove::doGoToPointLookBall(PlayerAgent *agent,
                                              const Vector2D &target_point,
                                              const AngleDeg &body_angle,
                                              const double &dist_thr,
                                              const double &dash_power,
                                              const double &back_power_rate)
{
    const WorldModel &wm = agent->world();

    if (wm.gameMode().type() == GameMode::PlayOn || wm.gameMode().type() == GameMode::PenaltyTaken_)
    {
        agent->debugClient().addMessage("Goalie:GoToLook");
        Bhv_GoToPointLookBall(target_point,
                              dist_thr,
                              dash_power,
                              back_power_rate)
            .execute(agent);
    }
    else
    {
        agent->debugClient().addMessage("Goalie:GoTo");
        if (Body_GoToPoint(target_point, dist_thr, dash_power).execute(agent))
        {
        }
        else
        {
            Body_TurnToAngle(body_angle).execute(agent);
        }

        agent->setNeckAction(new Neck_TurnToBall());
    }
}
