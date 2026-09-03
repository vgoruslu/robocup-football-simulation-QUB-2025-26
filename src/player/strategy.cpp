// -*-c++-*-
/*!
  \file strategy.cpp
  \brief team strategy Source File
*/

/*
 *Copyright: Copyright (C) Hidehisa AKIYAMA
This code is free software; you can redistribute it and/or modify it under the terms
of the GNU General Public License as published by the Free Software Foundation;
either version 3, or (at your option) any later version.

This code is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with this code;
see the file COPYING. If not, write to the Free Software Foundation,
675 Mass Ave, Cambridge, MA 02139, USA.
*EndCopyright:
*/

/////////////////////////////////////////////////////////////////////

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "strategy.h"
#include "soccer_role.h"

#ifndef USE_GENERIC_FACTORY
#include "role_sample.h"
#include "role_center_back.h"
#include "role_center_forward.h"
#include "role_defensive_half.h"
#include "role_goalie.h"
#include "role_offensive_half.h"
#include "role_side_back.h"
#include "role_side_forward.h"
#include "role_side_half.h"
#include "role_keepaway_keeper.h"
#include "role_keepaway_taker.h"
#endif

#include <rcsc/player/intercept_table.h>
#include <rcsc/player/world_model.h>
#include <rcsc/formation/formation_parser.h>
#include <rcsc/common/server_param.h>
#include <rcsc/param/cmd_line_parser.h>
#include <rcsc/param/param_map.h>
#include <rcsc/game_mode.h>
#include <iostream>

using namespace rcsc;

const std::string Strategy::BEFORE_KICK_OFF_CONF = "before-kick-off.conf";
const std::string Strategy::NORMAL_FORMATION_CONF = "normal-formation.conf";
const std::string Strategy::DEFENSE_FORMATION_CONF = "defense-formation.conf";
const std::string Strategy::OFFENSE_FORMATION_CONF = "offense-formation.conf";
const std::string Strategy::GOAL_KICK_OPP_FORMATION_CONF = "goal-kick-opp.conf";
const std::string Strategy::GOAL_KICK_OUR_FORMATION_CONF = "goal-kick-our.conf";
const std::string Strategy::GOALIE_CATCH_OPP_FORMATION_CONF = "goalie-catch-opp.conf";
const std::string Strategy::GOALIE_CATCH_OUR_FORMATION_CONF = "goalie-catch-our.conf";
const std::string Strategy::KICKIN_OUR_FORMATION_CONF = "kickin-our-formation.conf";
const std::string Strategy::SETPLAY_OPP_FORMATION_CONF = "setplay-opp-formation.conf";
const std::string Strategy::SETPLAY_OUR_FORMATION_CONF = "setplay-our-formation.conf";
const std::string Strategy::INDIRECT_FREEKICK_OPP_FORMATION_CONF = "indirect-freekick-opp-formation.conf";
const std::string Strategy::INDIRECT_FREEKICK_OUR_FORMATION_CONF = "indirect-freekick-our-formation.conf";

Strategy::Strategy()
    : M_goalie_unum(Unum_Unknown),
      M_current_situation(Normal_Situation),
      M_role_number(11, 0),
      M_position_types(11, Position_Center),
      M_positions(11)
{
#ifndef USE_GENERIC_FACTORY
    M_role_factory[RoleSample::name()] = &RoleSample::create;
    M_role_factory[RoleGoalie::name()] = &RoleGoalie::create;
    M_role_factory[RoleCenterBack::name()] = &RoleCenterBack::create;
    M_role_factory[RoleSideBack::name()] = &RoleSideBack::create;
    M_role_factory[RoleDefensiveHalf::name()] = &RoleDefensiveHalf::create;
    M_role_factory[RoleOffensiveHalf::name()] = &RoleOffensiveHalf::create;
    M_role_factory[RoleSideHalf::name()] = &RoleSideHalf::create;
    M_role_factory[RoleSideForward::name()] = &RoleSideForward::create;
    M_role_factory[RoleCenterForward::name()] = &RoleCenterForward::create;

    M_role_factory[RoleKeepawayKeeper::name()] = &RoleKeepawayKeeper::create;
    M_role_factory[RoleKeepawayTaker::name()] = &RoleKeepawayTaker::create;
#endif

    for (size_t i = 0; i < M_role_number.size(); ++i)
    {
        M_role_number[i] = i + 1;
    }
}

Strategy &
Strategy::instance()
{
    static Strategy s_instance;
    return s_instance;
}

bool Strategy::init(CmdLineParser &cmd_parser)
{
    ParamMap param_map("HELIOS_base options");

    if (cmd_parser.count("help") > 0)
    {
        param_map.printHelp(std::cout);
        return false;
    }

    cmd_parser.parse(param_map);

    return true;
}

bool Strategy::read(const std::string &formation_dir)
{
    static bool s_initialized = false;
    if (s_initialized)
    {
        std::cerr << __FILE__ << ' ' << __LINE__
                  << ": already initialized." << std::endl;
        return false;
    }

    std::string configpath = formation_dir;
    if (!configpath.empty() && configpath[configpath.length() - 1] != '/')
    {
        configpath += '/';
    }

    M_before_kick_off_formation = createFormation(configpath + BEFORE_KICK_OFF_CONF);
    if (!M_before_kick_off_formation)
    {
        std::cerr << "Failed to read before_kick_off formation" << std::endl;
        return false;
    }

    M_normal_formation = createFormation(configpath + NORMAL_FORMATION_CONF);
    if (!M_normal_formation)
    {
        std::cerr << "Failed to read normal formation" << std::endl;
        return false;
    }

    M_defense_formation = createFormation(configpath + DEFENSE_FORMATION_CONF);
    if (!M_defense_formation)
    {
        std::cerr << "Failed to read defense formation" << std::endl;
        return false;
    }

    M_offense_formation = createFormation(configpath + OFFENSE_FORMATION_CONF);
    if (!M_offense_formation)
    {
        std::cerr << "Failed to read offense formation" << std::endl;
        return false;
    }

    M_goal_kick_opp_formation = createFormation(configpath + GOAL_KICK_OPP_FORMATION_CONF);
    if (!M_goal_kick_opp_formation)
    {
        return false;
    }

    M_goal_kick_our_formation = createFormation(configpath + GOAL_KICK_OUR_FORMATION_CONF);
    if (!M_goal_kick_our_formation)
    {
        return false;
    }

    M_goalie_catch_opp_formation = createFormation(configpath + GOALIE_CATCH_OPP_FORMATION_CONF);
    if (!M_goalie_catch_opp_formation)
    {
        return false;
    }

    M_goalie_catch_our_formation = createFormation(configpath + GOALIE_CATCH_OUR_FORMATION_CONF);
    if (!M_goalie_catch_our_formation)
    {
        return false;
    }

    M_kickin_our_formation = createFormation(configpath + KICKIN_OUR_FORMATION_CONF);
    if (!M_kickin_our_formation)
    {
        std::cerr << "Failed to read kickin our formation" << std::endl;
        return false;
    }

    M_setplay_opp_formation = createFormation(configpath + SETPLAY_OPP_FORMATION_CONF);
    if (!M_setplay_opp_formation)
    {
        std::cerr << "Failed to read setplay opp formation" << std::endl;
        return false;
    }

    M_setplay_our_formation = createFormation(configpath + SETPLAY_OUR_FORMATION_CONF);
    if (!M_setplay_our_formation)
    {
        std::cerr << "Failed to read setplay our formation" << std::endl;
        return false;
    }

    M_indirect_freekick_opp_formation = createFormation(configpath + INDIRECT_FREEKICK_OPP_FORMATION_CONF);
    if (!M_indirect_freekick_opp_formation)
    {
        std::cerr << "Failed to read indirect freekick opp formation" << std::endl;
        return false;
    }

    M_indirect_freekick_our_formation = createFormation(configpath + INDIRECT_FREEKICK_OUR_FORMATION_CONF);
    if (!M_indirect_freekick_our_formation)
    {
        std::cerr << "Failed to read indirect freekick our formation" << std::endl;
        return false;
    }

    s_initialized = true;
    return true;
}

Formation::Ptr
Strategy::createFormation(const std::string &filepath)
{
    Formation::Ptr f = FormationParser::parse(filepath);
    if (!f)
    {
        std::cerr << "(Strategy::createFormation) Could not create a formation from "
                  << filepath << std::endl;
        return Formation::Ptr();
    }

    for (int unum = 1; unum <= 11; ++unum)
    {
        const std::string role_name = f->roleName(unum);

        if (role_name == "Savior" || role_name == "Goalie")
        {
            if (M_goalie_unum == Unum_Unknown)
            {
                M_goalie_unum = unum;
            }

            if (M_goalie_unum != unum)
            {
                std::cerr << __FILE__ << ':' << __LINE__ << ':'
                          << " ***ERROR*** Illegal goalie's uniform number"
                          << " read unum=" << unum
                          << " expected=" << M_goalie_unum
                          << std::endl;
                f.reset();
                return f;
            }
        }

#ifdef USE_GENERIC_FACTORY
        SoccerRole::Ptr role = SoccerRole::create(role_name);
        if (!role)
        {
            std::cerr << __FILE__ << ':' << __LINE__ << ':'
                      << " ***ERROR*** Unsupported role name ["
                      << role_name << "] is appered in ["
                      << filepath << "]" << std::endl;
            f.reset();
            return f;
        }
#else
        if (M_role_factory.find(role_name) == M_role_factory.end())
        {
            std::cerr << __FILE__ << ':' << __LINE__ << ':'
                      << " ***ERROR*** Unsupported role name ["
                      << role_name << "] is appered in ["
                      << filepath << "]" << std::endl;
            f.reset();
            return f;
        }
#endif
    }

    return f;
}

void Strategy::update(const WorldModel &wm)
{
    static GameTime s_update_time(-1, 0);
    if (s_update_time == wm.time())
    {
        return;
    }

    s_update_time = wm.time();

    updateSituation(wm);
    updatePosition(wm);
}

void Strategy::exchangeRole(const int unum0,
                            const int unum1)
{
    if (unum0 < 1 || 11 < unum0 || unum1 < 1 || 11 < unum1)
    {
        std::cerr << __FILE__ << ':' << __LINE__ << ':'
                  << "(exchangeRole) Illegal uniform number. "
                  << unum0 << ' ' << unum1 << std::endl;
        return;
    }

    if (unum0 == unum1)
    {
        std::cerr << __FILE__ << ':' << __LINE__ << ':'
                  << "(exchangeRole) same uniform number. "
                  << unum0 << ' ' << unum1 << std::endl;
        return;
    }

    int role0 = M_role_number[unum0 - 1];
    int role1 = M_role_number[unum1 - 1];

    M_role_number[unum0 - 1] = role1;
    M_role_number[unum1 - 1] = role0;
}

bool Strategy::isMarkerType(const int unum) const
{
    int number = roleNumber(unum);

    if (number == 2 || number == 3 || number == 4 || number == 5)
    {
        return true;
    }

    return false;
}

SoccerRole::Ptr
Strategy::createRole(const int unum,
                     const WorldModel &world) const
{
    const int number = roleNumber(unum);
    SoccerRole::Ptr role;

    if (number < 1 || 11 < number)
    {
        std::cerr << __FILE__ << ": " << __LINE__
                  << " ***ERROR*** Invalid player number "
                  << number << std::endl;
        return role;
    }

    Formation::Ptr f = getFormation(world);
    if (!f)
    {
        std::cerr << __FILE__ << ": " << __LINE__
                  << " ***ERROR*** faled to create role. Null formation"
                  << std::endl;
        return role;
    }

    const std::string role_name = f->roleName(number);

#ifdef USE_GENERIC_FACTORY
    role = SoccerRole::create(role_name);
#else
    RoleFactory::const_iterator factory = M_role_factory.find(role_name);
    if (factory != M_role_factory.end())
    {
        role = factory->second();
    }
#endif

    if (!role)
    {
        std::cerr << __FILE__ << ": " << __LINE__
                  << " ***ERROR*** unsupported role name ["
                  << role_name << "]" << std::endl;
    }

    return role;
}

void Strategy::updateSituation(const WorldModel &wm)
{
    M_current_situation = Normal_Situation;

    if (wm.gameMode().type() != GameMode::PlayOn)
    {
        if (wm.gameMode().isPenaltyKickMode())
        {
            M_current_situation = PenaltyKick_Situation;
        }
        else if (wm.gameMode().isOurSetPlay(wm.ourSide()))
        {
            M_current_situation = OurSetPlay_Situation;
        }
        else
        {
            M_current_situation = OppSetPlay_Situation;
        }
        return;
    }

    int self_min = wm.interceptTable().selfStep();
    int mate_min = wm.interceptTable().teammateStep();
    int opp_min = wm.interceptTable().opponentStep();

    int our_min = std::min(self_min, mate_min);

    if (opp_min <= our_min - 2)
    {
        M_current_situation = Defense_Situation;
        return;
    }

    if (our_min <= opp_min - 2)
    {
        M_current_situation = Offense_Situation;
        return;
    }
}

void Strategy::updatePosition(const WorldModel &wm)
{
    static GameTime s_update_time(0, 0);
    if (s_update_time == wm.time())
    {
        return;
    }
    s_update_time = wm.time();

    Formation::Ptr f = getFormation(wm);
    if (!f)
    {
        std::cerr << wm.teamName() << ':'
                  << wm.self().unum() << ": "
                  << wm.time()
                  << " ***ERROR*** could not get the current formation"
                  << std::endl;
        return;
    }

    int ball_step = 0;
    if (wm.gameMode().type() == GameMode::PlayOn || wm.gameMode().type() == GameMode::GoalKick_)
    {
        ball_step = std::min(1000, wm.interceptTable().teammateStep());
        ball_step = std::min(ball_step, wm.interceptTable().opponentStep());
        ball_step = std::min(ball_step, wm.interceptTable().selfStep());
    }

    Vector2D ball_pos = wm.ball().inertiaPoint(ball_step);

    M_positions.clear();
    f->getPositions(ball_pos, M_positions);

    if (ServerParam::i().useOffside())
    {
        double max_x = wm.offsideLineX();

        if (ServerParam::i().kickoffOffside() && (wm.gameMode().type() == GameMode::BeforeKickOff || wm.gameMode().type() == GameMode::AfterGoal_))
        {
            max_x = 0.0;
        }
        else
        {
            int mate_step = wm.interceptTable().teammateStep();
            if (mate_step < 50)
            {
                Vector2D trap_pos = wm.ball().inertiaPoint(mate_step);
                if (trap_pos.x > max_x)
                    max_x = trap_pos.x;
            }
            max_x -= 1.0;
        }

        for (int unum = 1; unum <= 11; ++unum)
        {
            if (M_positions[unum - 1].x > max_x)
            {
                M_positions[unum - 1].x = max_x;
            }
        }
    }

    if (M_current_situation == Offense_Situation)
    {
        Vector2D ball = wm.ball().pos();

        // Player 8 acts as the main central support option in advanced attacks.
        if (ball.x > 5.0)
        {
            M_positions[7].x = rcsc::bound(12.0, ball.x + 2.5, 40.0);
            M_positions[7].y = rcsc::bound(-7.0, ball.y * 0.10, 7.0);

            if (std::fabs(ball.y) > 8.0)
            {
                M_positions[7].x = rcsc::bound(26.0, ball.x - 2.0, 43.0);
                M_positions[7].y = (ball.y > 0.0 ? ball.y - 6.0 : ball.y + 6.0);
                M_positions[7].y = rcsc::bound(-6.0, M_positions[7].y, 6.0);
            }
        }

        // Player 7 stays underneath the ball as the safer recycle option.
        if (ball.x > 5.0)
        {
            M_positions[6].x = rcsc::bound(-5.0, ball.x - 8.0, 34.0);
            M_positions[6].y = rcsc::bound(-10.0, ball.y * 0.25, 10.0);
        }

        if (ball.x > 24.0 && std::fabs(ball.y) < 10.0)
        {
            M_positions[7].x = rcsc::bound(28.0, ball.x + 2.0, 43.0);
            M_positions[7].y = (ball.y >= 0.0 ? -2.5 : 2.5);
        }

        if (ball.x > 34.0 && std::fabs(ball.y) > 8.0)
        {
            enum WideSupportMode
            {
                WIDE_BYLINE,
                WIDE_INSIDE,
                WIDE_RECYCLE
            };

            WideSupportMode mode = WIDE_BYLINE;

            int nearby_opps = 0;
            int central_box_blockers = 0;
            double nearest_opp = 100.0;

            for (auto o = wm.opponentsFromBall().begin();
                 o != wm.opponentsFromBall().end(); ++o)
            {
                if (!(*o))
                    continue;

                const Vector2D op = (*o)->pos();
                const double d = op.dist(ball);

                if (d < nearest_opp)
                    nearest_opp = d;
                if (d < 6.0)
                    ++nearby_opps;

                if (op.x > ball.x - 6.0 && op.x < 49.0 && std::fabs(op.y) < 10.0)
                {
                    ++central_box_blockers;
                }
            }

            if (nearest_opp < 2.5 || nearby_opps >= 3 || central_box_blockers >= 3)
            {
                mode = WIDE_RECYCLE;
            }
            else if (ball.x < 43.0 && std::fabs(ball.y) > 7.0 && std::fabs(ball.y) < 16.0)
            {
                mode = WIDE_INSIDE;
            }
            else
            {
                mode = WIDE_BYLINE;
            }

            if (mode == WIDE_BYLINE)
            {
                M_positions[10].x = rcsc::bound(37.0, ball.x - 1.0, 48.5);

                if (ball.y > 0.0)
                    M_positions[10].y = rcsc::bound(1.0, ball.y - 3.0, 8.0);
                else
                    M_positions[10].y = rcsc::bound(-8.0, ball.y + 3.0, -1.0);

                M_positions[7].x = rcsc::bound(33.0, ball.x - 3.0, 45.5);
                M_positions[7].y = (ball.y > 0.0 ? -2.5 : 2.5);

                if (ball.y > 0.0)
                {
                    M_positions[8].x = rcsc::bound(33.0, ball.x - 3.0, 47.0);
                    M_positions[8].y = -6.0;
                }
                else
                {
                    M_positions[9].x = rcsc::bound(33.0, ball.x - 3.0, 47.0);
                    M_positions[9].y = 6.0;
                }

                M_positions[6].x = rcsc::bound(26.0, ball.x - 8.0, 38.0);
                M_positions[6].y = (ball.y > 0.0 ? -5.5 : 5.5);
            }
            else if (mode == WIDE_INSIDE)
            {
                M_positions[10].x = rcsc::bound(37.0, ball.x - 1.5, 47.5);
                M_positions[10].y = (ball.y > 0.0 ? 1.5 : -1.5);

                M_positions[7].x = rcsc::bound(31.0, ball.x - 1.5, 43.5);
                M_positions[7].y = (ball.y > 0.0 ? ball.y - 5.5 : ball.y + 5.5);
                M_positions[7].y = rcsc::bound(-5.5, M_positions[7].y, 5.5);

                M_positions[6].x = rcsc::bound(24.0, ball.x - 8.0, 36.0);
                M_positions[6].y = (ball.y > 0.0 ? ball.y - 10.0 : ball.y + 10.0);
                M_positions[6].y = rcsc::bound(-8.0, M_positions[6].y, 8.0);

                if (ball.y > 0.0)
                {
                    M_positions[8].x = rcsc::bound(28.0, ball.x - 7.0, 40.0);
                    M_positions[8].y = -10.0;
                }
                else
                {
                    M_positions[9].x = rcsc::bound(28.0, ball.x - 7.0, 40.0);
                    M_positions[9].y = 10.0;
                }
            }
            else
            {
                M_positions[10].x = rcsc::bound(35.0, ball.x - 3.0, 45.5);
                M_positions[10].y = (ball.y > 0.0 ? 2.5 : -2.5);

                M_positions[7].x = rcsc::bound(28.0, ball.x - 6.0, 39.0);
                M_positions[7].y = (ball.y > 0.0 ? -2.0 : 2.0);

                M_positions[6].x = rcsc::bound(22.0, ball.x - 10.0, 33.0);
                M_positions[6].y = (ball.y > 0.0 ? 5.5 : -5.5);

                if (ball.y > 0.0)
                {
                    M_positions[8].x = rcsc::bound(22.0, ball.x - 11.0, 35.0);
                    M_positions[8].y = -13.0;
                }
                else
                {
                    M_positions[9].x = rcsc::bound(22.0, ball.x - 11.0, 35.0);
                    M_positions[9].y = 13.0;
                }
            }
        }
    }

    M_position_types.clear();

    for (int unum = 1; unum <= 11; ++unum)
    {
        PositionType type = Position_Center;

        const RoleType role_type = f->roleType(unum);
        if (role_type.side() == RoleType::Left)
        {
            type = Position_Left;
        }
        else if (role_type.side() == RoleType::Right)
        {
            type = Position_Right;
        }

        M_position_types.push_back(type);
    }
}

PositionType
Strategy::getPositionType(const int unum) const
{
    const int number = roleNumber(unum);

    if (number < 1 || 11 < number)
    {
        std::cerr << __FILE__ << ' ' << __LINE__
                  << ": Illegal number : " << number << std::endl;
        return Position_Center;
    }

    try
    {
        return M_position_types.at(number - 1);
    }
    catch (std::exception &e)
    {
        std::cerr << __FILE__ << ':' << __LINE__ << ':'
                  << " Exception caught! " << e.what() << std::endl;
        return Position_Center;
    }
}

Vector2D
Strategy::getPosition(const int unum) const
{
    const int number = roleNumber(unum);

    if (number < 1 || 11 < number)
    {
        std::cerr << __FILE__ << ' ' << __LINE__
                  << ": Illegal number : " << number << std::endl;
        return Vector2D::INVALIDATED;
    }

    try
    {
        return M_positions.at(number - 1);
    }
    catch (std::exception &e)
    {
        std::cerr << __FILE__ << ':' << __LINE__ << ':'
                  << " Exception caught! " << e.what() << std::endl;
        return Vector2D::INVALIDATED;
    }
}

Formation::Ptr
Strategy::getFormation(const WorldModel &wm) const
{
    if (wm.gameMode().type() == GameMode::PlayOn)
    {
        switch (M_current_situation)
        {
        case Defense_Situation:
            return M_defense_formation;
        case Offense_Situation:
            return M_offense_formation;
        default:
            return M_normal_formation;
        }
    }

    if (wm.gameMode().type() == GameMode::KickIn_ || wm.gameMode().type() == GameMode::CornerKick_)
    {
        if (wm.ourSide() == wm.gameMode().side())
        {
            return M_kickin_our_formation;
        }
        else
        {
            return M_setplay_opp_formation;
        }
    }

    if ((wm.gameMode().type() == GameMode::BackPass_ && wm.gameMode().side() == wm.theirSide()) || (wm.gameMode().type() == GameMode::IndFreeKick_ && wm.gameMode().side() == wm.ourSide()))
    {
        return M_indirect_freekick_our_formation;
    }

    if ((wm.gameMode().type() == GameMode::BackPass_ && wm.gameMode().side() == wm.ourSide()) || (wm.gameMode().type() == GameMode::IndFreeKick_ && wm.gameMode().side() == wm.theirSide()))
    {
        return M_indirect_freekick_opp_formation;
    }

    if (wm.gameMode().type() == GameMode::FoulCharge_ || wm.gameMode().type() == GameMode::FoulPush_)
    {
        if (wm.gameMode().side() == wm.ourSide())
        {
            if (wm.ball().pos().x < ServerParam::i().ourPenaltyAreaLineX() + 1.0 && wm.ball().pos().absY() < ServerParam::i().penaltyAreaHalfWidth() + 1.0)
            {
                return M_indirect_freekick_opp_formation;
            }
            else
            {
                return M_setplay_opp_formation;
            }
        }
        else
        {
            if (wm.ball().pos().x > ServerParam::i().theirPenaltyAreaLineX() && wm.ball().pos().absY() < ServerParam::i().penaltyAreaHalfWidth())
            {
                return M_indirect_freekick_our_formation;
            }
            else
            {
                return M_setplay_our_formation;
            }
        }
    }

    if (wm.gameMode().type() == GameMode::GoalKick_)
    {
        if (wm.gameMode().side() == wm.ourSide())
        {
            return M_goal_kick_our_formation;
        }
        else
        {
            return M_goal_kick_opp_formation;
        }
    }

    if (wm.gameMode().type() == GameMode::GoalieCatch_)
    {
        if (wm.gameMode().side() == wm.ourSide())
        {
            return M_goalie_catch_our_formation;
        }
        else
        {
            return M_goalie_catch_opp_formation;
        }
    }

    if (wm.gameMode().type() == GameMode::BeforeKickOff || wm.gameMode().type() == GameMode::AfterGoal_)
    {
        return M_before_kick_off_formation;
    }

    if (wm.gameMode().isOurSetPlay(wm.ourSide()))
    {
        return M_setplay_our_formation;
    }

    if (wm.gameMode().type() != GameMode::PlayOn)
    {
        return M_setplay_opp_formation;
    }

    switch (M_current_situation)
    {
    case Defense_Situation:
        return M_defense_formation;
    case Offense_Situation:
        return M_offense_formation;
    default:
        break;
    }

    return M_normal_formation;
}

Strategy::BallArea
Strategy::get_ball_area(const WorldModel &wm)
{
    int ball_step = 1000;
    ball_step = std::min(ball_step, wm.interceptTable().teammateStep());
    ball_step = std::min(ball_step, wm.interceptTable().opponentStep());
    ball_step = std::min(ball_step, wm.interceptTable().selfStep());

    return get_ball_area(wm.ball().inertiaPoint(ball_step));
}

Strategy::BallArea
Strategy::get_ball_area(const Vector2D &ball_pos)
{
    if (ball_pos.x > 36.0)
    {
        if (ball_pos.absY() > 17.0)
        {
            return BA_Cross;
        }
        else
        {
            return BA_ShootChance;
        }
    }
    else if (ball_pos.x > -1.0)
    {
        if (ball_pos.absY() > 17.0)
        {
            return BA_DribbleAttack;
        }
        else
        {
            return BA_OffMidField;
        }
    }
    else if (ball_pos.x > -30.0)
    {
        if (ball_pos.absY() > 17.0)
        {
            return BA_DribbleBlock;
        }
        else
        {
            return BA_DefMidField;
        }
    }
    else if (ball_pos.x > -35.5)
    {
        if (ball_pos.absY() > 17.0)
        {
            return BA_CrossBlock;
        }
        else
        {
            return BA_DefMidField;
        }
    }
    else
    {
        if (ball_pos.absY() > 17.0)
        {
            return BA_CrossBlock;
        }
        else
        {
            return BA_Danger;
        }
    }

    return BA_None;
}

double
Strategy::get_normal_dash_power(const WorldModel &wm)
{
    static bool s_recover_mode = false;

    if (wm.self().staminaModel().capacityIsEmpty())
    {
        return std::min(ServerParam::i().maxDashPower(),
                        wm.self().stamina() + wm.self().playerType().extraStamina());
    }

    const int self_min = wm.interceptTable().selfStep();
    const int mate_min = wm.interceptTable().teammateStep();
    const int opp_min = wm.interceptTable().opponentStep();

    if (wm.self().staminaModel().capacityIsEmpty())
    {
        s_recover_mode = false;
    }
    else if (wm.self().stamina() < ServerParam::i().staminaMax() * 0.5)
    {
        s_recover_mode = true;
    }
    else if (wm.self().stamina() > ServerParam::i().staminaMax() * 0.7)
    {
        s_recover_mode = false;
    }

    double dash_power = ServerParam::i().maxDashPower();
    const double my_inc = wm.self().playerType().staminaIncMax() * wm.self().recovery();

    if (wm.ourDefenseLineX() > wm.self().pos().x && wm.ball().pos().x < wm.ourDefenseLineX() + 20.0)
    {
        dash_power = ServerParam::i().maxDashPower();
    }
    else if (s_recover_mode)
    {
        dash_power = my_inc - 25.0;
        if (dash_power < 0.0)
            dash_power = 0.0;
    }
    else if (wm.kickableTeammate() && wm.ball().distFromSelf() < 20.0)
    {
        dash_power = std::min(my_inc * 1.1,
                              ServerParam::i().maxDashPower());
    }
    else if (wm.self().pos().x > wm.offsideLineX())
    {
        dash_power = ServerParam::i().maxDashPower();
    }
    else if (wm.ball().pos().x > 25.0 && wm.ball().pos().x > wm.self().pos().x + 10.0 && self_min < opp_min - 6 && mate_min < opp_min - 6)
    {
        dash_power = bound(ServerParam::i().maxDashPower() * 0.1,
                           my_inc * 0.5,
                           ServerParam::i().maxDashPower());
    }
    else
    {
        dash_power = std::min(my_inc * 1.7,
                              ServerParam::i().maxDashPower());
    }

    return dash_power;
}
