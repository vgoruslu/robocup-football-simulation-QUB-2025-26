// -*-c++-*-

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "role_offensive_half.h"

#include "bhv_basic_move.h"

#include "planner/bhv_planned_action.h"
#include "basic_actions/body_hold_ball.h"
#include "basic_actions/neck_scan_field.h"
#include "basic_actions/body_dribble2008.h"
#include "basic_actions/body_smart_kick.h"
#include "basic_actions/body_go_to_point.h"

#include <rcsc/player/world_model.h>
#include <rcsc/player/player_agent.h>
#include <rcsc/player/debug_client.h>

#include <rcsc/common/server_param.h>

#include <algorithm>
#include <cmath>

using namespace rcsc;

const std::string RoleOffensiveHalf::NAME("OffensiveHalf");

namespace
{

    rcss::RegHolder role = SoccerRole::creators().autoReg(&RoleOffensiveHalf::create,
                                                          RoleOffensiveHalf::NAME);

    double nearestOpponentToSelf(const WorldModel &wm)
    {
        double nearest = 100.0;

        for (auto o = wm.opponentsFromSelf().begin();
             o != wm.opponentsFromSelf().end(); ++o)
        {
            if (!(*o))
                continue;
            nearest = std::min(nearest, (*o)->distFromSelf());
        }

        return nearest;
    }

    const AbstractPlayerObject *findOpponentGoalie(const WorldModel &wm)
    {
        for (auto o = wm.opponentsFromSelf().begin();
             o != wm.opponentsFromSelf().end(); ++o)
        {
            if (!(*o))
                continue;
            if ((*o)->goalie())
                return *o;
        }

        return nullptr;
    }

    bool laneIsDead(const WorldModel &wm,
                    const Vector2D &from,
                    const Vector2D &to)
    {
        const Vector2D line = to - from;
        const double len2 = line.r2();

        if (len2 < 0.0001)
            return true;

        for (auto o = wm.opponentsFromBall().begin();
             o != wm.opponentsFromBall().end(); ++o)
        {
            if (!(*o))
                continue;

            const Vector2D op = (*o)->pos();
            const double proj = ((op - from).innerProduct(line)) / len2;

            if (proj < 0.08 || proj > 0.92)
                continue;

            const Vector2D foot = from + line * proj;
            const double perp = op.dist(foot);

            if (perp < 1.0)
                return true;
        }

        return false;
    }

    double laneBlockPenalty(const WorldModel &wm,
                            const Vector2D &from,
                            const Vector2D &to)
    {
        const Vector2D line = to - from;
        const double len2 = line.r2();

        if (len2 < 0.0001)
            return 10.0;

        double penalty = 0.0;

        for (auto o = wm.opponentsFromBall().begin();
             o != wm.opponentsFromBall().end(); ++o)
        {
            if (!(*o))
                continue;

            const Vector2D op = (*o)->pos();
            const double proj = ((op - from).innerProduct(line)) / len2;

            if (proj < 0.05 || proj > 0.95)
                continue;

            const Vector2D foot = from + line * proj;
            const double perp = op.dist(foot);

            if (perp < 1.0)
                penalty += 7.0;
            else if (perp < 2.0)
                penalty += 3.5;
            else if (perp < 3.0)
                penalty += (3.0 - perp) * 1.2;
        }

        return penalty;
    }

    int countForwardBlockers(const WorldModel &wm,
                             const Vector2D &ball,
                             const double x_ahead,
                             const double y_width)
    {
        int blockers = 0;

        for (auto o = wm.opponentsFromBall().begin();
             o != wm.opponentsFromBall().end(); ++o)
        {
            if (!(*o))
                continue;

            const Vector2D op = (*o)->pos();

            if (op.x > ball.x && op.x < ball.x + x_ahead && std::fabs(op.y - ball.y) < y_width)
            {
                ++blockers;
            }
        }

        return blockers;
    }

    bool isScoringZone(const Vector2D &p)
    {
        return p.x > 28.0 && std::fabs(p.y) < 12.0;
    }

    bool inStrikerLane(const Vector2D &p)
    {
        return p.x > 28.0 && std::fabs(p.y) < 7.0;
    }

    bool isTransitionZone(const Vector2D &p)
    {
        return p.x > 0.0 && p.x < 30.0;
    }

    bool isVeryShortPass(const Vector2D &from, const Vector2D &to)
    {
        return from.dist(to) < 5.0;
    }

    bool isSameCrowdedLane(const Vector2D &from, const Vector2D &to)
    {
        return std::fabs(from.y - to.y) < 4.0 && std::fabs(from.y) < 10.0 && std::fabs(to.y) < 10.0;
    }

    double transitionProgressBonus(const Vector2D &from, const Vector2D &to)
    {
        double bonus = 0.0;

        bonus += std::max(0.0, to.x - from.x) * 0.18;

        if (std::fabs(from.y) < 8.0 && std::fabs(to.y) > 10.0)
        {
            bonus += 2.2;
        }

        if (std::fabs(to.y) >= 6.0 && std::fabs(to.y) <= 14.0 && to.x > from.x + 3.0)
        {
            bonus += 1.3;
        }

        return bonus;
    }

    double poorShortPassPenalty(const AbstractPlayerObject *target,
                                const Vector2D &ball,
                                const bool attacking_target)
    {
        if (!target)
            return 0.0;

        const Vector2D pos = target->pos();
        double penalty = 0.0;

        if (isVeryShortPass(ball, pos))
            penalty += 3.0;

        if (isSameCrowdedLane(ball, pos))
            penalty += 2.5;

        if (target->unum() == 11 && ball.dist(pos) < 6.5 && std::fabs(pos.y - ball.y) < 5.5)
        {
            penalty += 6.0;
        }

        if (isTransitionZone(ball) && ball.dist(pos) < 6.5 && std::fabs(pos.y) < 10.0)
        {
            penalty += 2.2;
        }

        if (!attacking_target && target->unum() == 11 && inStrikerLane(pos))
        {
            penalty += 3.0;
        }

        return penalty;
    }

    double evalTarget(const WorldModel &wm,
                      const Vector2D &ball,
                      const AbstractPlayerObject *goalie,
                      const AbstractPlayerObject *target,
                      const double base_score,
                      const bool attacking_target)
    {
        if (!target)
            return -1000.0;

        const Vector2D pos = target->pos();

        if (laneIsDead(wm, ball, pos))
            return -1000.0;

        double score = base_score;

        score += pos.x * 0.10;
        score -= std::fabs(pos.y) * 0.08;
        score -= ball.dist(pos) * 0.05;
        score -= laneBlockPenalty(wm, ball, pos);

        int close_markers = 0;

        for (auto o = wm.opponentsFromBall().begin();
             o != wm.opponentsFromBall().end(); ++o)
        {
            if (!(*o))
                continue;

            if ((*o)->pos().dist(pos) < 2.5)
                ++close_markers;
        }

        score -= close_markers * 2.0;

        if (attacking_target && goalie)
        {
            const double gd = goalie->pos().dist(pos);
            if (gd < 6.0)
                score -= (6.0 - gd) * 0.8;
        }

        score += transitionProgressBonus(ball, pos);
        score -= poorShortPassPenalty(target, ball, attacking_target);

        return score;
    }

    Vector2D makeCamDribbleTarget(const WorldModel &wm)
    {
        const Vector2D ball = wm.ball().pos();
        const double self_y = wm.self().pos().y;

        double target_x = ball.x + 5.0;
        double target_y = 0.0;

        if (std::fabs(ball.y) < 6.0)
        {
            target_y = (self_y >= 0.0 ? 6.5 : -6.5);
        }
        else
        {
            target_y = ball.y * 0.80;
        }

        if (ball.x > 24.0)
        {
            target_x = ball.x + 4.0;
            target_y = (ball.y >= 0.0
                            ? std::max(3.0, ball.y * 0.70)
                            : std::min(-3.0, ball.y * 0.70));
        }

        target_y = rcsc::bound(-14.0, target_y, 14.0);
        target_x = rcsc::bound(ball.x + 2.0, target_x, 46.0);

        return Vector2D(target_x, target_y);
    }

}

bool RoleOffensiveHalf::execute(PlayerAgent *agent)
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
        doKick(agent);
    else
        doMove(agent);

    return true;
}

void RoleOffensiveHalf::doKick(PlayerAgent *agent)
{
    const WorldModel &wm = agent->world();
    const Vector2D ball = wm.ball().pos();

    // Leave the other attacking half on the default planner.
    if (wm.self().unum() != 8)
    {
        if (Bhv_PlannedAction().execute(agent))
        {
            agent->debugClient().addMessage("PlannedAction");
            return;
        }

        Body_HoldBall().execute(agent);
        agent->setNeckAction(new Neck_ScanField());
        return;
    }

    // Avoid forcing central play from deep positions.
    if (ball.x <= -2.0)
    {
        if (Bhv_PlannedAction().execute(agent))
        {
            agent->debugClient().addMessage("PlannedAction");
            return;
        }

        Body_HoldBall().execute(agent);
        agent->setNeckAction(new Neck_ScanField());
        return;
    }

    const AbstractPlayerObject *through_target = nullptr;
    const AbstractPlayerObject *slip_target = nullptr;
    const AbstractPlayerObject *wide_target = nullptr;
    const AbstractPlayerObject *recycle_target = nullptr;
    const AbstractPlayerObject *goalie = findOpponentGoalie(wm);

    const double safe_offside_x = wm.offsideLineX() - 1.0;

    for (auto t = wm.teammatesFromBall().begin();
         t != wm.teammatesFromBall().end(); ++t)
    {
        if (!(*t))
            continue;
        if ((*t)->unum() == wm.self().unum())
            continue;

        const Vector2D pos = (*t)->pos();
        const double dist = ball.dist(pos);

        if (pos.x > safe_offside_x)
            continue;

        if (pos.x > ball.x + 5.5
            && pos.x < safe_offside_x
            && std::fabs(pos.y) >= 5.0
            && std::fabs(pos.y) <= 15.0
            && dist > 7.0)
        {
            if (!through_target
                || evalTarget(wm, ball, goalie, *t, 0.0, true)
                       > evalTarget(wm, ball, goalie, through_target, 0.0, true))
            {
                through_target = *t;
            }
        }

        if (pos.x > ball.x - 1.5
            && pos.x < ball.x + 9.0
            && std::fabs(pos.y) < std::max(4.0, std::fabs(ball.y) + 1.0)
            && dist < 13.0)
        {
            if (!slip_target
                || evalTarget(wm, ball, goalie, *t, 0.0, true)
                       > evalTarget(wm, ball, goalie, slip_target, 0.0, true))
            {
                slip_target = *t;
            }
        }

        if (pos.x > ball.x - 6.0
            && pos.x < ball.x + 14.0
            && std::fabs(pos.y) > 11.0
            && dist > 6.0)
        {
            if (!wide_target
                || evalTarget(wm, ball, goalie, *t, 0.0, false)
                       > evalTarget(wm, ball, goalie, wide_target, 0.0, false))
            {
                wide_target = *t;
            }
        }

        if (pos.x > ball.x - 14.0
            && pos.x < ball.x + 3.0
            && std::fabs(pos.y) < 18.0)
        {
            if (!recycle_target
                || evalTarget(wm, ball, goalie, *t, 0.0, false)
                       > evalTarget(wm, ball, goalie, recycle_target, 0.0, false))
            {
                recycle_target = *t;
            }
        }
    }

    const double nearest_opp_to_self = nearestOpponentToSelf(wm);
    const int forward_blockers = countForwardBlockers(wm, ball, 7.0, 4.5);

    const bool scoring_zone = isScoringZone(ball);
    const bool heavy_pressure =
        (nearest_opp_to_self < 2.2) || (countForwardBlockers(wm, ball, 5.0, 5.0) >= 2);

    if (scoring_zone)
    {
        double shot_score = 7.0;
        shot_score += (ball.x - 28.0) * 0.40;
        shot_score -= std::fabs(ball.y) * 0.18;
        shot_score -= forward_blockers * 1.0;
        if (nearest_opp_to_self > 2.6)
            shot_score += 1.0;

        double slip_score = evalTarget(wm, ball, goalie, slip_target, 6.5, true);
        double through_score = evalTarget(wm, ball, goalie, through_target, 6.0, true);

        if (through_target)
        {
            through_score += (through_target->pos().x - ball.x) * 0.10;
        }

        if (shot_score > slip_score && shot_score > through_score && shot_score > 7.2)
        {
            Vector2D goal_target(52.5, (ball.y >= 0.0 ? -2.2 : 2.2));

            if (Body_SmartKick(goal_target, 2.8, 1.0, 3).execute(agent))
            {
                agent->debugClient().addMessage("Shot");
                agent->setNeckAction(new Neck_ScanField());
                return;
            }
        }
    }

    if (heavy_pressure)
    {
        if (wide_target && !laneIsDead(wm, ball, wide_target->pos()))
        {
            const double dist = ball.dist(wide_target->pos());
            const double speed = std::min(2.4, std::max(1.4, 1.0 + dist * 0.05));

            if (Body_SmartKick(wide_target->pos(), speed, 1.0, 2).execute(agent))
            {
                agent->debugClient().addMessage("WidePass");
                agent->setNeckAction(new Neck_ScanField());
                return;
            }
        }

        if (recycle_target && !laneIsDead(wm, ball, recycle_target->pos()))
        {
            const double dist = ball.dist(recycle_target->pos());
            const double speed = std::min(2.2, std::max(1.3, 1.0 + dist * 0.04));

            if (Body_SmartKick(recycle_target->pos(), speed, 1.0, 2).execute(agent))
            {
                agent->debugClient().addMessage("Recycle");
                agent->setNeckAction(new Neck_ScanField());
                return;
            }
        }
    }

    double through_score = evalTarget(wm, ball, goalie, through_target, 6.0, true);
    double slip_score = evalTarget(wm, ball, goalie, slip_target, 5.8, true);
    double wide_score = evalTarget(wm, ball, goalie, wide_target, 6.2, false);
    double recycle_score = evalTarget(wm, ball, goalie, recycle_target, 4.5, false);

    if (through_target)
    {
        through_score += (through_target->pos().x - ball.x) * 0.10;
    }

    double dribble_score = 7.0;
    if (nearest_opp_to_self > 3.0)
        dribble_score += 1.5;
    else if (nearest_opp_to_self > 2.5)
        dribble_score += 0.7;
    else if (nearest_opp_to_self < 2.0)
        dribble_score -= 2.0;

    dribble_score -= forward_blockers * 0.8;
    dribble_score -= std::fabs(ball.y) * 0.05;

    // Only play the killer pass when it clearly beats carrying the ball.
    if (through_target
        && through_score > 7.4
        && through_score > dribble_score + 1.0
        && !laneIsDead(wm, ball, through_target->pos()))
    {
        Vector2D pass_target = through_target->pos() + through_target->vel() * 2.0;
        pass_target.x += 1.5;
        if (pass_target.x > safe_offside_x)
            pass_target.x = safe_offside_x;

        const double dist = ball.dist(pass_target);
        const double speed = std::min(2.7, std::max(1.6, 1.2 + dist * 0.05));

        if (Body_SmartKick(pass_target, speed, 1.0, 2).execute(agent))
        {
            agent->debugClient().addMessage("ThroughPass");
            agent->setNeckAction(new Neck_ScanField());
            return;
        }
    }

    if (slip_target
        && slip_score > 6.8
        && slip_score > dribble_score + 0.6
        && !laneIsDead(wm, ball, slip_target->pos()))
    {
        const double dist = ball.dist(slip_target->pos());
        const double speed = std::min(2.4, std::max(1.4, 1.1 + dist * 0.05));

        if (Body_SmartKick(slip_target->pos(), speed, 1.0, 2).execute(agent))
        {
            agent->debugClient().addMessage("SlipPass");
            agent->setNeckAction(new Neck_ScanField());
            return;
        }
    }

    if (dribble_score > wide_score && dribble_score > recycle_score && dribble_score > 6.5)
    {
        const Vector2D dribble_target = makeCamDribbleTarget(wm);

        if (Body_Dribble2008(dribble_target,
                             1.0,
                             ServerParam::i().maxDashPower(),
                             6)
                .execute(agent))
        {
            agent->debugClient().addMessage("Carry");
            agent->setNeckAction(new Neck_ScanField());
            return;
        }
    }

    if (wide_target && !laneIsDead(wm, ball, wide_target->pos()) && wide_score > 6.0)
    {
        const double dist = ball.dist(wide_target->pos());
        const double speed = std::min(2.4, std::max(1.4, 1.0 + dist * 0.05));

        if (Body_SmartKick(wide_target->pos(), speed, 1.0, 2).execute(agent))
        {
            agent->debugClient().addMessage("WidePass");
            agent->setNeckAction(new Neck_ScanField());
            return;
        }
    }

    if (recycle_target && !laneIsDead(wm, ball, recycle_target->pos()) && recycle_score > 5.5)
    {
        const double dist = ball.dist(recycle_target->pos());
        const double speed = std::min(2.2, std::max(1.3, 1.0 + dist * 0.04));

        if (Body_SmartKick(recycle_target->pos(), speed, 1.0, 2).execute(agent))
        {
            agent->debugClient().addMessage("Recycle");
            agent->setNeckAction(new Neck_ScanField());
            return;
        }
    }

    if (Bhv_PlannedAction().execute(agent))
    {
        agent->debugClient().addMessage("PlannedAction");
        return;
    }

    Body_HoldBall().execute(agent);
    agent->debugClient().addMessage("Hold");
    agent->setNeckAction(new Neck_ScanField());
}

void RoleOffensiveHalf::doMove(PlayerAgent *agent)
{
    Bhv_BasicMove().execute(agent);
}
