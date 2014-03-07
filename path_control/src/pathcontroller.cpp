#include "pathcontroller.h"

using namespace path_msgs;

PathController::PathController(ros::NodeHandle &nh):
    node_handle_(nh),
    navigate_to_goal_server_(nh, "navigate_to_goal", boost::bind(&PathController::navToGoalActionCallback, this, _1), false),
    follow_path_client_("follow_path"),
    goal_timestamp_(ros::Time(0)),
    unexpected_path_(false)
{
    ROS_INFO("Wait for follow_path action server...");
    follow_path_client_.waitForServer();

    ros::param::param<float>("~nonaction_velocity", opt_.unexpected_path_velocity, 0.5);

    goal_pub_ = nh.advertise<geometry_msgs::PoseStamped>("/move_base_simple/goal", 0);

    navigate_to_goal_server_.start();

    path_sub_ = nh.subscribe<nav_msgs::Path>("/path", 10, &PathController::pathCallback, this);

    ROS_INFO("Initialisation done.");
}

void PathController::navToGoalActionCallback(const path_msgs::NavigateToGoalGoalConstPtr &goal)
{
    ROS_INFO("Start Action!! [%d]", goal->debug_test);

    if (unexpected_path_) {
        ROS_INFO("Cancel execution of unexpected path.");
        follow_path_client_.cancelGoal();
    }

    current_goal_ = goal;

    switch (goal->failure_mode) {
    case NavigateToGoalGoal::FAILURE_MODE_ABORT:
        /// Abort mode. Simply process ones and abort, if some problem occurs.
        processGoal();
        handleFollowPathResult();
        break;

    case NavigateToGoalGoal::FAILURE_MODE_REPLAN:
        /// Replan mode. If some problem occurs during path following, make new plan with same goal.
        while(true) {
            if (!processGoal()) {
                // Follower aborted or goal got preempted. We are finished here, result is already send.
                return;
            }

            // if follower reports success, we are done. If not, replan
            if (follow_path_result_->status == FollowPathResult::MOTION_STATUS_SUCCESS) {
                break;
            } else {
                ROS_WARN("Path execution failed. Replan.");
                // send feedback
                NavigateToGoalFeedback feedback;
                feedback.status = NavigateToGoalFeedback::STATUS_REPLAN;
                navigate_to_goal_server_.publishFeedback(feedback);
            }
        }

        handleFollowPathResult();
        break;

    default:
        ROS_ERROR("Invalid failure mode %d.", goal->failure_mode);
        NavigateToGoalResult result;
        result.status = NavigateToGoalResult::STATUS_OTHER_ERROR;
        navigate_to_goal_server_.setAborted(result, "Invalid failure mode.");
        break;
    }
}

bool PathController::processGoal()
{
    follow_path_done_ = false;

    // send goal pose to planner and wait for the result
    waitForPath(current_goal_->goal_pose);

    // check, if path has been found
    if ( requested_path_->poses.size() < 2 ) {
        ROS_WARN("Got an invalid path with less than two poses. Abort goal.");

        NavigateToGoalResult result;
        result.reached_goal = false;
        result.status = NavigateToGoalResult::STATUS_NO_PATH_FOUND;

        navigate_to_goal_server_.setAborted(result);

        return false;
    }

    // before we're continuing, check if the goal already has been preemted to avoid unnecessary start of follow_path
    // action
    if (navigate_to_goal_server_.isPreemptRequested()) {
        ROS_INFO("Preempt goal [%d].\n---------------------", current_goal_->debug_test);
        navigate_to_goal_server_.setPreempted();
        return false;
    }

    // feedback about path
    {
        NavigateToGoalFeedback feedback;
        feedback.status = NavigateToGoalFeedback::STATUS_PATH_READY;
        navigate_to_goal_server_.publishFeedback(feedback);
    }

    path_msgs::FollowPathGoal path_action_goal;
    path_action_goal.debug_test = current_goal_->debug_test;
    path_action_goal.path = *requested_path_;
    path_action_goal.velocity = current_goal_->velocity;

    follow_path_client_.sendGoal(path_action_goal,
                                 boost::bind(&PathController::followPathDoneCB, this, _1, _2),
                                 boost::bind(&PathController::followPathActiveCB, this),
                                 boost::bind(&PathController::followPathFeedbackCB, this, _1));

    while ( ! follow_path_client_.getState().isDone() ) {
        if (navigate_to_goal_server_.isPreemptRequested()) {
            ROS_INFO("Preempt goal [%d].\n---------------------", current_goal_->debug_test);
            follow_path_client_.cancelGoal();
            // wait until the goal is really canceled (= done callback is called).
            if (!waitForFollowPathDone(ros::Duration(10))) {
                ROS_WARN("follow_path_client does not react to cancelGoal() for 10 seconds.");
            }

            navigate_to_goal_server_.setPreempted();

            // don't check for new goal here. If there is one, it will cause a new execution of this callback, after
            // this instance has stopped.
            return false;
        }

        // As long as only one action client is active, a new goal should automatically preempt the former goal.
        // Separately checking for new goals should only be necessary, if there are more than one clients (or a client
        // that gets restarted), which is currently not intended.
//        if (navigate_to_goal_server_.isNewGoalAvailable()) {
//            ROS_INFO("New goal available [%d].\n---------------------", goal->debug_test);
//            follow_path_client_.cancelGoal();
//            navigate_to_goal_server_.setPreempted();
//            break;
//        }
    }


    // wait until the action is really finished
    if (!waitForFollowPathDone(ros::Duration(10))) {
        ROS_WARN("Wait for follow_path action to be finished, but timeout expired!");
        NavigateToGoalResult result;
        result.status = NavigateToGoalResult::STATUS_TIMEOUT;
        navigate_to_goal_server_.setAborted(result, "Wait for follow_path action to be finished, but timeout expired.");
        return false;
    }
    return true;
}

void PathController::handleFollowPathResult()
{
    /*** IMPORTANT: No matter, what the result is, the navigate_to_goal action has to be finished in some way! ***/

    /// Construct result message
    path_msgs::NavigateToGoalResult nav_result;

    nav_result.reached_goal = (follow_path_result_->status == FollowPathResult::MOTION_STATUS_SUCCESS);
    nav_result.debug_test = follow_path_result_->debug_test;

    ROS_DEBUG("FollowPathResult status = %d", follow_path_result_->status);

    if (nav_result.reached_goal) {
        nav_result.status = NavigateToGoalResult::STATUS_SUCCESS;
    }
    else {
        switch (follow_path_result_->status) {
        case FollowPathResult::MOTION_STATUS_COLLISION:
            nav_result.status = NavigateToGoalResult::STATUS_COLLISION;
            break;

        case FollowPathResult::MOTION_STATUS_PATH_LOST:
            nav_result.status = NavigateToGoalResult::STATUS_LOST_PATH;
            break;

        case FollowPathResult::MOTION_STATUS_TIMEOUT:
            nav_result.status = NavigateToGoalResult::STATUS_TIMEOUT;
            break;

        default:
            nav_result.status = NavigateToGoalResult::STATUS_OTHER_ERROR;
            break;
        }
    }


    /* Terminate navigate_to_goal action according to the final state of the the follow_path action.
     *
     * According to [1] only REJECTED, RECALLED, PREEMPTED, ABORTED and SUCCEEDED are terminal states.
     * Thus theses states should be the only ones, that can occur here.
     *
     * [1] http://wiki.ros.org/actionlib/DetailedDescription
     */
    switch (follow_path_final_state_) {
    case GoalState::REJECTED:
    case GoalState::RECALLED:
    case GoalState::ABORTED:
        navigate_to_goal_server_.setAborted(nav_result);
        break;

    case GoalState::PREEMPTED:
        // This should never happen, because this method should not be called when the goal was preemted (this is
        // handled separately in the execute-callback).
        ROS_ERROR("This function should never receive a preemted goal. This is likely a bug! [file %s, line %d]",
                  __FILE__, __LINE__);
        navigate_to_goal_server_.setAborted(nav_result);
        break;

    case GoalState::SUCCEEDED:
        navigate_to_goal_server_.setSucceeded(nav_result);
        break;

    default: // Are there other states, that should be handled somehow?
        ROS_ERROR("Unexpected final state of follow_path goal. navigate_to_goal is aborted. Maybe this is a bug. [file %s, line %d]",
                  __FILE__, __LINE__);
        navigate_to_goal_server_.setAborted(nav_result);

    }
}

void PathController::pathCallback(const nav_msgs::PathConstPtr &path)
{
    if (goal_timestamp_.isZero()) { // unexpected path -> case 2

        // unexpected paths are not allowed to preempt regular action-based goals
        if (!navigate_to_goal_server_.isActive()) {
            ROS_INFO("Execute unexpected path.");
            unexpected_path_ = true;

            path_msgs::FollowPathGoal path_action_goal;
            path_action_goal.debug_test = 255;
            path_action_goal.path = *path;
            path_action_goal.velocity = opt_.unexpected_path_velocity;

            // only simple callback that resets unexpected_path_, feedback is ignored.
            follow_path_client_.sendGoal(path_action_goal,
                                         boost::bind(&PathController::followUnexpectedPathDoneCB, this, _1, _2));
        } else {
            ROS_DEBUG("Unexpected path omitted.");
        }

    } else { // expected path -> case 1

        if (path->header.stamp == goal_timestamp_) {
            requested_path_ = path;
            // reset to 0 to signalise, that there is no outstanding path
            goal_timestamp_ = ros::Time(0);
        }
        // else: drop this path (= do nothing)
    }
}

void PathController::followPathDoneCB(const actionlib::SimpleClientGoalState &state,
                                      const path_msgs::FollowPathResultConstPtr &result)
{
    ROS_INFO("Path execution finished [%d].\n---------------------", result->debug_test);

    follow_path_final_state_ = state.state_;
    follow_path_result_ = result;
    follow_path_done_ = true;
}

void PathController::followPathActiveCB()
{
    ROS_INFO("Path is now active.");
    // is there anything to do here?
}

void PathController::followPathFeedbackCB(const path_msgs::FollowPathFeedbackConstPtr &feedback)
{
    //ROS_INFO_THROTTLE(1,"Driven distance: %g;  Distance to goal: %g", feedback->dist_driven, feedback->dist_goal);

    path_msgs::NavigateToGoalFeedback nav_feedback;
    nav_feedback.debug_test = feedback->debug_test;

    switch(feedback->status) {
    case FollowPathFeedback::MOTION_STATUS_MOVING:
        nav_feedback.status = NavigateToGoalFeedback::STATUS_MOVING;
        break;

    case FollowPathFeedback::MOTION_STATUS_COLLISION:
        nav_feedback.status = NavigateToGoalFeedback::STATUS_COLLISION;
        break;

    default:
        ROS_ERROR("Feedback: Unknown status code %d", feedback->status);
        break;
    }

    navigate_to_goal_server_.publishFeedback(nav_feedback);
}

void PathController::followUnexpectedPathDoneCB(const actionlib::SimpleClientGoalState &state,
                                                const path_msgs::FollowPathResultConstPtr &result)
{
    ROS_INFO("Execution of unexpected path finished [%d, %s].\n---------------------",
             result->debug_test,state.toString().c_str());
    unexpected_path_ = false;
}

void PathController::waitForPath(const geometry_msgs::PoseStamped &goal_pose)
{
    //TODO: Can there be concurrency problems? I think not, but better think a bit more deeply about it.

    //TODO: Timeout? Not so urgent here, as a new goal will abort waiting.
    //TODO: Necessary to check for new goals? - I think yes.

    goal_timestamp_ = goal_pose.header.stamp;
    goal_pub_.publish(goal_pose);

    ROS_DEBUG("Wait for path...");
    while (!goal_timestamp_.isZero()
           && ros::ok()
           && !navigate_to_goal_server_.isPreemptRequested()
           && !navigate_to_goal_server_.isNewGoalAvailable())
    { }
    ROS_DEBUG("Stop waiting (stamp: %d;   ok: %d;   preempt: %d;   new goal: %d)",
              !goal_timestamp_.isZero(),
              ros::ok(),
              !navigate_to_goal_server_.isPreemptRequested(),
              !navigate_to_goal_server_.isNewGoalAvailable());
}

bool PathController::waitForFollowPathDone(ros::Duration timeout)
{
    ros::Time expire_time = ros::Time::now() + timeout;

    while (!follow_path_done_ && expire_time > ros::Time::now());

    return follow_path_done_;
}
