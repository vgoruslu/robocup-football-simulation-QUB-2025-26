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

#include "sample_player.h"

#include "strategy.h"
#include "field_analyzer.h"

#include "action_chain_holder.h"
#include "sample_field_evaluator.h"

#include "soccer_role.h"

#include "sample_communication.h"
#include "keepaway_communication.h"
#include "sample_freeform_message_parser.h"

#include "bhv_penalty_kick.h"
#include "bhv_set_play.h"
#include "bhv_set_play_kick_in.h"
#include "bhv_set_play_indirect_free_kick.h"

#include "bhv_custom_before_kick_off.h"
#include "bhv_strict_check_shoot.h"

#include "view_tactical.h"

#include "intention_receive.h"

#include "basic_actions/basic_actions.h"
#include "basic_actions/bhv_emergency.h"
#include "basic_actions/body_go_to_point.h"
#include "basic_actions/body_intercept.h"
#include "basic_actions/body_kick_one_step.h"
#include "basic_actions/neck_scan_field.h"
#include "basic_actions/neck_turn_to_ball_or_scan.h"
#include "basic_actions/view_synch.h"
#include "basic_actions/kick_table.h"

#include <rcsc/formation/formation.h>
#include <rcsc/player/intercept_table.h>
#include <rcsc/player/say_message_builder.h>
#include <rcsc/player/audio_sensor.h>

#include <rcsc/common/abstract_client.h>
#include <rcsc/common/server_param.h>
#include <rcsc/common/player_param.h>
#include <rcsc/common/audio_memory.h>
#include <rcsc/common/say_message_parser.h>

#include <rcsc/param/param_map.h>
#include <rcsc/param/cmd_line_parser.h>

#include <iostream>
#include <sstream>
#include <string>
#include <cstdlib>

using namespace rcsc;

/*-------------------------------------------------------------------*/
/*!

 */
// Constructor: builds base player (PlayerAgent) and initialises member variables,
// then sets up decision-making and communication systems
SamplePlayer::SamplePlayer()
    : PlayerAgent(),
      M_communication()
{

    // Set up decision-making components so they are reasy to be used:
    // - action generator (produces possible actions)
    // - field evaluator (scores/ranks those actions)
    M_field_evaluator = createFieldEvaluator();
    M_action_generator = createActionGenerator();

    // Shared memory for storing all received communication (primarily from teammates),
    // which can be used to improve the agent's understanding of the game
    // These parsers below, read incoming messages and save useful info into shared memory
    std::shared_ptr<AudioMemory> audio_memory(new AudioMemory);

    // Give the world model access to the shared audio memory
    M_worldmodel.setAudioMemory(audio_memory);

    //
    // set communication message parser
    //
    // Each parser handles a specific type of message, extracts useful information,
    // and stores it in shared memory for the agent to use
    addSayMessageParser(new BallMessageParser(audio_memory));
    addSayMessageParser(new PassMessageParser(audio_memory));
    addSayMessageParser(new InterceptMessageParser(audio_memory));
    addSayMessageParser(new GoalieMessageParser(audio_memory));
    addSayMessageParser(new GoalieAndPlayerMessageParser(audio_memory));
    addSayMessageParser(new OffsideLineMessageParser(audio_memory));
    addSayMessageParser(new DefenseLineMessageParser(audio_memory));
    addSayMessageParser(new WaitRequestMessageParser(audio_memory));
    addSayMessageParser(new PassRequestMessageParser(audio_memory));
    addSayMessageParser(new DribbleMessageParser(audio_memory));
    addSayMessageParser(new BallGoalieMessageParser(audio_memory));
    addSayMessageParser(new OnePlayerMessageParser(audio_memory));
    addSayMessageParser(new TwoPlayerMessageParser(audio_memory));
    addSayMessageParser(new ThreePlayerMessageParser(audio_memory));
    addSayMessageParser(new SelfMessageParser(audio_memory));
    addSayMessageParser(new TeammateMessageParser(audio_memory));
    addSayMessageParser(new OpponentMessageParser(audio_memory));
    addSayMessageParser(new BallPlayerMessageParser(audio_memory));
    addSayMessageParser(new StaminaMessageParser(audio_memory));
    addSayMessageParser(new RecoveryMessageParser(audio_memory));

    // addSayMessageParser( new FreeMessageParser< 9 >( audio_memory ) );
    // addSayMessageParser( new FreeMessageParser< 8 >( audio_memory ) );
    // addSayMessageParser( new FreeMessageParser< 7 >( audio_memory ) );
    // addSayMessageParser( new FreeMessageParser< 6 >( audio_memory ) );
    // addSayMessageParser( new FreeMessageParser< 5 >( audio_memory ) );
    // addSayMessageParser( new FreeMessageParser< 4 >( audio_memory ) );
    // addSayMessageParser( new FreeMessageParser< 3 >( audio_memory ) );
    // addSayMessageParser( new FreeMessageParser< 2 >( audio_memory ) );
    // addSayMessageParser( new FreeMessageParser< 1 >( audio_memory ) );

    //
    // set freeform message parser
    //
    //// Handle messages that provide opponent player type info and update the world model directly
    addFreeformMessageParser(new OpponentPlayerTypeMessageParser(M_worldmodel));

    //
    // set communication planner
    //
    // Set up and store the communication system used to decide and send messages to teammates
    M_communication = Communication::Ptr(new SampleCommunication());
}

/*-------------------------------------------------------------------*/
/*!

 */
// Destructor: runs when the player shuts down (no manual cleanup needed)
SamplePlayer::~SamplePlayer()
{
}

/*-------------------------------------------------------------------*/
/*!

 */
bool SamplePlayer::initImpl(CmdLineParser &cmd_parser)
{
    // Run the PlayerAgent (framework) setup and store whether it succeeded
    // It sets up everything needed for the agent to connect to the server and function as a player in the game
    bool result = PlayerAgent::initImpl(cmd_parser);

    // read additional options
    // Initialise strategy system (may use command-line options if provided)
    result &= Strategy::instance().init(cmd_parser);

    // Placeholder for additional command-line options (not used here)
    rcsc::ParamMap my_params("Additional options");
#if 0
    std::string param_file_path = "params";
    param_map.add()
        ( "param-file", "", &param_file_path, "specified parameter file" );
#endif

    // Read any additional command-line options (none really used here)
    cmd_parser.parse(my_params);

    // If user asked for help, print help info and stop the program
    if (cmd_parser.count("help") > 0)
    {
        my_params.printHelp(std::cout);
        return false;
    }

    // If there were invalid/unsupported options, print a warning
    if (cmd_parser.failed())
    {
        std::cerr << "player: ***WARNING*** detected unsuppprted options: ";
        cmd_parser.print(std::cerr);
        std::cerr << std::endl;
    }

    // If earlier setup failed (framework or strategy init), stop
    if (!result)
    {
        return false;
    }

    // Get the global strategy object (Strategy::instance())
    // and try to load strategy data (formations, tactics) from the config directory.
    // If loading fails (read returns false), handle the error.
    if (!Strategy::instance().read(config().configDir()))
    {
        std::cerr << "***ERROR*** Failed to read team strategy." << std::endl;
        return false;
    }

    // Load kick data (used for accurate kicking decisions)
    if (KickTable::instance().read(config().configDir() + "/kick-table"))
    {
        std::cerr << "Loaded the kick table: ["
                  << config().configDir() << "/kick-table]"
                  << std::endl;
    }
    // Everything worked player is ready to start
    return true;
}

/*-------------------------------------------------------------------*/
/*!
// Main decision loop: runs every cycle to decide what the player should do
  main decision
  virtual method in super class
*/
void SamplePlayer::actionImpl()
{
    // If a trainer message was heard this cycle, print it.
    if (this->audioSensor().trainerMessageTime() == world().time())
    {
        std::cerr << world().ourTeamName() << ' ' << world().self().unum()
                  << ' ' << world().time()
                  << " receive trainer message["
                  << this->audioSensor().trainerMessage() << ']'
                  << std::endl;
    }

    //
    // update strategy and analyzer
    //
    // Update team strategy and field analysis using the latest world state so the player refreshes its understanding before deciding
    Strategy::instance().update(world());
    FieldAnalyzer::instance().update(world());

    //
    // prepare action chain
    //
    // This is setting up the tools used to
    // generate possible actions and evaluate them
    // store/use them in the action chain system so the player can compare possible actions
    M_field_evaluator = createFieldEvaluator();
    M_action_generator = createActionGenerator();

    ActionChainHolder::instance().setFieldEvaluator(M_field_evaluator);
    ActionChainHolder::instance().setActionGenerator(M_action_generator);

    //
    // special situations (tackle, objects accuracy, intention...)
    //
    // Run preprocess to handle urgent cases:
    // - frozen (tackle)
    // - kickoff/reset positioning
    // - invalid self position
    // - lost ball (search)
    // - shooting opportunity
    // - continue previous intention
    // - forced kick situations
    // - pass communication
    // If any of these run, stop the rest of this cycle
    if (doPreprocess())
    {
        return;
    }

    //
    // update action chain
    //
    // Generate possible actions and evaluate them using the current world state for this cycle
    ActionChainHolder::instance().update(world());

    //
    // create current role
    //
    // Pointer to hold this player's role (behaviour for this cycle)
    SoccerRole::Ptr role_ptr;
    {
        // Ask the strategy system to create this player's role based on:
        // - their player number (unum)
        // - the current world state (positions, ball, game mode, etc.)
        role_ptr = Strategy::i().createRole(world().self().unum(), world());

        if (!role_ptr)
        {
            // if no role returned print error
            std::cerr << config().teamName() << ": "
                      << world().self().unum()
                      << " Error. Role is not registerd.\nExit ..."
                      << std::endl;
            M_client->setServerAlive(false);
            // end this cycle
            return;
        }
    }

    //
    // coordinated press
    //
    if (doCoordinatedHighPress())
    {
        return;
    }

    //
    // override execute if role accept
    //
    // Does the role want to compeletly take over
    if (role_ptr->acceptExecution(world()))
    {
        // if yes execute the role specific code
        role_ptr->execute(this);
        return;
    }

    //
    // play_on mode
    //
    if (world().gameMode().type() == GameMode::PlayOn)
    {
        role_ptr->execute(this);
        return;
    }

    //
    // penalty kick mode
    //
    if (world().gameMode().isPenaltyKickMode())
    {
        Bhv_PenaltyKick().execute(this);
        return;
    }

    //
    // other set play mode
    //
    Bhv_SetPlay().execute(this);
}

/*-------------------------------------------------------------------*/
/*!

 */
void SamplePlayer::handleActionStart()
{
}

/*-------------------------------------------------------------------*/
/*!

 */
void SamplePlayer::handleActionEnd()
{
    if (world().self().posValid())
    {
#if 0
        const ServerParam & SP = ServerParam::i();
        //
        // inside of pitch
        //

        // top,lower
        debugClient().addLine( Vector2D( world().ourOffenseLineX(),
                                         -SP.pitchHalfWidth() ),
                               Vector2D( world().ourOffenseLineX(),
                                         -SP.pitchHalfWidth() + 3.0 ) );
        // top,lower
        debugClient().addLine( Vector2D( world().ourDefenseLineX(),
                                         -SP.pitchHalfWidth() ),
                               Vector2D( world().ourDefenseLineX(),
                                         -SP.pitchHalfWidth() + 3.0 ) );

        // bottom,upper
        debugClient().addLine( Vector2D( world().theirOffenseLineX(),
                                         +SP.pitchHalfWidth() - 3.0 ),
                               Vector2D( world().theirOffenseLineX(),
                                         +SP.pitchHalfWidth() ) );
        //
        debugClient().addLine( Vector2D( world().offsideLineX(),
                                         world().self().pos().y - 15.0 ),
                               Vector2D( world().offsideLineX(),
                                         world().self().pos().y + 15.0 ) );

        // outside of pitch

        // top,upper
        debugClient().addLine( Vector2D( world().ourOffensePlayerLineX(),
                                         -SP.pitchHalfWidth() - 3.0 ),
                               Vector2D( world().ourOffensePlayerLineX(),
                                         -SP.pitchHalfWidth() ) );
        // top,upper
        debugClient().addLine( Vector2D( world().ourDefensePlayerLineX(),
                                         -SP.pitchHalfWidth() - 3.0 ),
                               Vector2D( world().ourDefensePlayerLineX(),
                                         -SP.pitchHalfWidth() ) );
        // bottom,lower
        debugClient().addLine( Vector2D( world().theirOffensePlayerLineX(),
                                         +SP.pitchHalfWidth() ),
                               Vector2D( world().theirOffensePlayerLineX(),
                                         +SP.pitchHalfWidth() + 3.0 ) );
        // bottom,lower
        debugClient().addLine( Vector2D( world().theirDefensePlayerLineX(),
                                         +SP.pitchHalfWidth() ),
                               Vector2D( world().theirDefensePlayerLineX(),
                                         +SP.pitchHalfWidth() + 3.0 ) );
#else
        // top,lower
        debugClient().addLine(Vector2D(world().ourDefenseLineX(),
                                       world().self().pos().y - 2.0),
                              Vector2D(world().ourDefenseLineX(),
                                       world().self().pos().y + 2.0));

        //
        debugClient().addLine(Vector2D(world().offsideLineX(),
                                       world().self().pos().y - 15.0),
                              Vector2D(world().offsideLineX(),
                                       world().self().pos().y + 15.0));
#endif
    }
}

/*-------------------------------------------------------------------*/
/*!

 */
void SamplePlayer::handleInitMessage()
{
    {
        // Initializing the order of penalty kickers
        std::vector<int> unum_order_pk_kickers = {10, 9, 2, 11, 3, 4, 1, 5, 6, 7, 8};
        M_worldmodel.setPenaltyKickTakerOrder(unum_order_pk_kickers);
    }
}

/*-------------------------------------------------------------------*/
/*!

 */
void SamplePlayer::handleServerParam()
{
    if (ServerParam::i().keepawayMode())
    {
        std::cerr << "set Keepaway mode communication." << std::endl;
        M_communication = Communication::Ptr(new KeepawayCommunication());
    }
}

/*-------------------------------------------------------------------*/
/*!

 */
void SamplePlayer::handlePlayerParam()
{
    if (KickTable::instance().createTables())
    {
        std::cerr << world().teamName() << ' '
                  << world().self().unum() << ": "
                  << " KickTable created."
                  << std::endl;
    }
    else
    {
        std::cerr << world().teamName() << ' '
                  << world().self().unum() << ": "
                  << " KickTable failed..."
                  << std::endl;
        M_client->setServerAlive(false);
    }
}

/*-------------------------------------------------------------------*/
/*!

 */
void SamplePlayer::handlePlayerType()
{
}

/*-------------------------------------------------------------------*/
/*!
  communication decision.
  virtual method in super class
*/
void SamplePlayer::communicationImpl()
{
    if (M_communication)
    {
        M_communication->execute(this);
    }
}

/*-------------------------------------------------------------------*/
/*!
 */
bool SamplePlayer::doPreprocess()
{
    // check tackle expires
    // check self position accuracy
    // ball search
    // check queued intention
    // check simultaneous kick

    const WorldModel &wm = this->world();

    //
    // freezed by tackle effect
    //
    if (wm.self().isFrozen())
    {
        // face neck to ball
        this->setViewAction(new View_Tactical());
        this->setNeckAction(new Neck_TurnToBallOrScan(0));
        return true;
    }

    //
    // BeforeKickOff or AfterGoal. jump to the initial position
    //
    if (wm.gameMode().type() == GameMode::BeforeKickOff || wm.gameMode().type() == GameMode::AfterGoal_)
    {
        Vector2D move_point = Strategy::i().getPosition(wm.self().unum());
        Bhv_CustomBeforeKickOff(move_point).execute(this);
        this->setViewAction(new View_Tactical());
        return true;
    }

    //
    // self localization error
    //
    if (!wm.self().posValid())
    {
        Bhv_Emergency().execute(this); // includes change view
        return true;
    }

    //
    // ball localization error
    //
    const int count_thr = (wm.self().goalie()
                               ? 10
                               : 5);
    if (wm.ball().posCount() > count_thr || (wm.gameMode().type() != GameMode::PlayOn && wm.ball().seenPosCount() > count_thr + 10))
    {
        this->setViewAction(new View_Tactical());
        Bhv_NeckBodyToBall().execute(this);
        return true;
    }

    //
    // set default change view
    //

    this->setViewAction(new View_Tactical());

    //
    // check shoot chance
    //
    if (doShoot())
    {
        return true;
    }

    //
    // check queued action
    //
    if (this->doIntention())
    {
        return true;
    }

    //
    // check simultaneous kick
    //
    if (doForceKick())
    {
        return true;
    }

    //
    // check pass message
    //
    if (doHeardPassReceive())
    {
        return true;
    }

    return false;
}

/*-------------------------------------------------------------------*/
/*!

*/
bool SamplePlayer::doShoot()
{
    const WorldModel &wm = this->world();

    if (wm.gameMode().type() != GameMode::IndFreeKick_ && wm.time().stopped() == 0 && wm.self().isKickable() && Bhv_StrictCheckShoot().execute(this))
    {
        // reset intention
        this->setIntention(static_cast<SoccerIntention *>(0));
        return true;
    }

    return false;
}

/*-------------------------------------------------------------------*/

bool SamplePlayer::doCoordinatedHighPress()
{
    const WorldModel &wm = world();
    const ServerParam &SP = ServerParam::i();

    if (wm.gameMode().type() != GameMode::PlayOn)
        return false;

    if (wm.self().goalie())
        return false;

    if (wm.self().isKickable() || wm.kickableTeammate())
        return false;

    if (!wm.self().posValid())
        return false;

    const Vector2D ball = wm.ball().pos();
    const int my_unum = wm.self().unum();
    const int attack_dir = (wm.ourSide() == SideID::LEFT ? 1 : -1);

    // Only press when the ball is in the opponent half.
    if (ball.x * attack_dir < 24.0)
        return false;

    // Only press when an opponent is likely controlling the ball.
    if (wm.maybeKickableOpponent() == nullptr)
        return false;

    // Do not press if stamina is too low.
    if (wm.self().stamina() < SP.staminaMax() * 0.70)
        return false;

    // If I can reach the ball quickly, or I am the closest teammate,
    // let the normal tackle/intercept behaviour handle it.
    const int self_min = wm.interceptTable().selfStep();
    const int mate_min = wm.interceptTable().teammateStep();

    if (self_min <= 3 || self_min <= mate_min)
        return false;

    // Find first and second pressers
    // Only attacking players 7-11 can actively press.

    int first_presser = -1;
    int second_presser = -1;

    double first_dist = 10000.0;
    double second_dist = 10000.0;

    if (my_unum >= 7)
    {
        first_presser = my_unum;
        first_dist = wm.self().pos().dist(ball);
    }

    for (const PlayerObject *p : wm.teammatesFromBall())
    {
        if (!p)
            continue;

        if (!p->posValid())
            continue;

        if (p->goalie())
            continue;

        if (p->unum() < 7)
            continue;

        const double d = p->pos().dist(ball);

        if (d < first_dist)
        {
            second_presser = first_presser;
            second_dist = first_dist;

            first_presser = p->unum();
            first_dist = d;
        }
        else if (d < second_dist)
        {
            second_presser = p->unum();
            second_dist = d;
        }
    }

    // First presser: closest attacker pressures the ball

    if (my_unum == first_presser)
    {
        Vector2D target = ball;

        this->debugClient().addMessage("PRESS:1st");
        this->setViewAction(new View_Tactical());

        Body_GoToPoint(target, 0.8, SP.maxDashPower()).execute(this);

        this->setNeckAction(new Neck_TurnToBallOrScan(0));
        return true;
    }

    // Second presser: blocks inside passing lane

    if (my_unum == second_presser)
    {
        Vector2D our_goal(-attack_dir * SP.pitchHalfLength(), 0.0);
        Vector2D direction = our_goal - ball;

        if (direction.r() > 1.0)
        {
            direction.setLength(7.0);
        }

        Vector2D target = ball + direction;

        this->debugClient().addMessage("PRESS:2nd");
        this->setViewAction(new View_Tactical());

        Body_GoToPoint(target, 1.0, SP.maxDashPower() * 0.85).execute(this);

        this->setNeckAction(new Neck_TurnToBallOrScan(0));
        return true;
    }

    // Shadow striker: player 11 blocks backwards/inside pass

    if (my_unum == 11 && my_unum != first_presser && my_unum != second_presser)
    {
        Vector2D target = ball;

        target.x -= attack_dir * 6.0;
        target.y *= 0.40;

        this->debugClient().addMessage("PRESS:SHADOW_STRIKER");
        this->setViewAction(new View_Tactical());

        Body_GoToPoint(target, 1.0, SP.maxDashPower() * 0.70).execute(this);

        this->setNeckAction(new Neck_TurnToBallOrScan(0));
        return true;
    }

    // Midfield screening: players 7 and 8 block central passes

    if ((my_unum == 7 || my_unum == 8) && my_unum != first_presser && my_unum != second_presser)
    {
        Vector2D target = ball;

        target.x -= attack_dir * 8.0;
        target.y *= 0.55;

        this->debugClient().addMessage("PRESS:MID_SCREEN");
        this->setViewAction(new View_Tactical());

        Body_GoToPoint(target, 1.2, SP.maxDashPower() * 0.65).execute(this);

        this->setNeckAction(new Neck_TurnToBallOrScan(0));
        return true;
    }

    // Rest defence: everyone else keeps team shape

    Vector2D home = Strategy::i().getPosition(my_unum);
    Vector2D target = home;

    // Shift sideways toward the ball.
    target.y += (ball.y - home.y) * 0.20;

    // Defenders and defensive midfielders stay deeper.
    if (my_unum <= 6)
    {
        target.x += (ball.x - home.x) * 0.05;
    }
    // Other players squeeze toward the ball a bit more.
    else
    {
        target.x += (ball.x - home.x) * 0.15;
    }

    this->debugClient().addMessage("PRESS:REST");
    this->setViewAction(new View_Tactical());

    Body_GoToPoint(target, 1.5, SP.maxDashPower() * 0.60).execute(this);

    this->setNeckAction(new Neck_TurnToBallOrScan(0));
    return true;
}

/*-------------------------------------------------------------------*/
/*!
 */
bool SamplePlayer::doForceKick()
{
    const WorldModel &wm = this->world();

    if (wm.gameMode().type() == GameMode::PlayOn && !wm.self().goalie() && wm.self().isKickable() && wm.kickableOpponent())
    {
        this->debugClient().addMessage("SimultaneousKick");
        Vector2D goal_pos(ServerParam::i().pitchHalfLength(), 0.0);

        if (wm.self().pos().x > 36.0 && wm.self().pos().absY() > 10.0)
        {
            goal_pos.x = 45.0;
        }
        Body_KickOneStep(goal_pos,
                         ServerParam::i().ballSpeedMax())
            .execute(this);
        this->setNeckAction(new Neck_ScanField());
        return true;
    }

    return false;
}

/*-------------------------------------------------------------------*/
/*!

*/
bool SamplePlayer::doHeardPassReceive()
{
    const WorldModel &wm = this->world();

    if (wm.audioMemory().passTime() != wm.time() || wm.audioMemory().pass().empty() || wm.audioMemory().pass().front().receiver_ != wm.self().unum())
    {

        return false;
    }

    int self_min = wm.interceptTable().selfStep();
    Vector2D intercept_pos = wm.ball().inertiaPoint(self_min);
    Vector2D heard_pos = wm.audioMemory().pass().front().receive_pos_;

    if (!wm.kickableTeammate() && wm.ball().posCount() <= 1 && wm.ball().velCount() <= 1 && self_min < 20
        //&& intercept_pos.dist( heard_pos ) < 3.0 ) //5.0 )
    )
    {
        this->debugClient().addMessage("Comm:Receive:Intercept");
        Body_Intercept().execute(this);
        this->setNeckAction(new Neck_TurnToBall());
    }
    else
    {
        this->debugClient().setTarget(heard_pos);
        this->debugClient().addMessage("Comm:Receive:GoTo");
        Body_GoToPoint(heard_pos,
                       0.5,
                       ServerParam::i().maxDashPower())
            .execute(this);
        this->setNeckAction(new Neck_TurnToBall());
    }

    this->setIntention(new IntentionReceive(heard_pos,
                                            ServerParam::i().maxDashPower(),
                                            0.9,
                                            5,
                                            wm.time()));

    return true;
}

/*-------------------------------------------------------------------*/
/*!

*/
FieldEvaluator::ConstPtr
SamplePlayer::getFieldEvaluator() const
{
    return M_field_evaluator;
}

/*-------------------------------------------------------------------*/
/*!

*/
FieldEvaluator::ConstPtr
SamplePlayer::createFieldEvaluator() const
{
    return FieldEvaluator::ConstPtr(new SampleFieldEvaluator);
}

/*-------------------------------------------------------------------*/
/*!
 */
#include "actgen_cross.h"
#include "actgen_direct_pass.h"
#include "actgen_self_pass.h"
#include "actgen_strict_check_pass.h"
#include "actgen_short_dribble.h"
#include "actgen_simple_dribble.h"
#include "actgen_shoot.h"
#include "actgen_action_chain_length_filter.h"

ActionGenerator::ConstPtr
SamplePlayer::createActionGenerator() const
{
    CompositeActionGenerator *g = new CompositeActionGenerator();

    //
    // shoot
    //
    g->addGenerator(new ActGen_RangeActionChainLengthFilter(new ActGen_Shoot(),
                                                            2, ActGen_RangeActionChainLengthFilter::MAX));

    //
    // strict check pass
    //
    g->addGenerator(new ActGen_MaxActionChainLengthFilter(new ActGen_StrictCheckPass(), 1));

    //
    // cross
    //
    g->addGenerator(new ActGen_MaxActionChainLengthFilter(new ActGen_Cross(), 1));

    //
    // direct pass
    //
    // g->addGenerator( new ActGen_RangeActionChainLengthFilter
    //                  ( new ActGen_DirectPass(),
    //                    2, ActGen_RangeActionChainLengthFilter::MAX ) );

    //
    // short dribble
    //
    g->addGenerator(new ActGen_MaxActionChainLengthFilter(new ActGen_ShortDribble(), 1));

    //
    // self pass (long dribble)
    //
    g->addGenerator(new ActGen_MaxActionChainLengthFilter(new ActGen_SelfPass(), 1));

    //
    // simple dribble
    //
    // g->addGenerator( new ActGen_RangeActionChainLengthFilter
    //                  ( new ActGen_SimpleDribble(),
    //                    2, ActGen_RangeActionChainLengthFilter::MAX ) );

    return ActionGenerator::ConstPtr(g);
}
