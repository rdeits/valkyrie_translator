/**

Plugin to feed core state and commands to and from the Valkyrie ros_control API.
Listens for torque commands for the torque controlled joints, and position commands
for the position controlled joints, and feeds them to the robot.
Forwards IMU, force/torque, and joint state over LCM in appropriate status messages.

Runs at 500hz in the Valkyrie ros_control main loop as a plugin.

Significant reference to
https://github.com/NASA-JSC-Robotics/valkyrie/wiki/Running-Controllers-on-Valkyrie

gizatt@mit.edu, 201601**
wolfgang.merkt@ed.ac.uk, 201603**

 **/

#include "LCM2ROSControl.hpp"

inline double clamp(double x, double lower, double upper) {
  return std::max(lower, std::min(upper, x));
}

namespace valkyrie_translator
{
 LCM2ROSControl::LCM2ROSControl()
 {}

 LCM2ROSControl::~LCM2ROSControl()
 {}

 bool LCM2ROSControl::initRequest(hardware_interface::RobotHW* robot_hw,
   ros::NodeHandle& root_nh, ros::NodeHandle& controller_nh,
   std::set<std::string>& claimed_resources)
 {
        // check if construction finished cleanly
  if (state_ != CONSTRUCTED){
    ROS_ERROR("Cannot initialize this controller because it failed to be constructed");
    return false;
  }

  if (!controller_nh.getParam("publish_core_robot_state", publishCoreRobotState)) {
    ROS_WARN("Could not read desired setting for publishing CORE_ROBOT_STATE, defaulting to true");
    publishCoreRobotState = true;
  }
  if (!controller_nh.getParam("publish_est_robot_state", publish_est_robot_state)) {
    ROS_WARN("Could not read desired setting for publishing EST_ROBOT_STATE, defaulting to false");
    publish_est_robot_state = false;
  }
  if (!controller_nh.getParam("apply_commands", applyCommands)) {
    ROS_WARN("Could not read desired setting for applying actual commands to the robot, defaulting to false");
    applyCommands = false;
  }

        // setup LCM (todo: move to constructor? how to propagate an error then?)
  lcm_ = boost::shared_ptr<lcm::LCM>(new lcm::LCM);
  if (!lcm_->good())
  {
    std::cerr << "ERROR: lcm is not good()" << std::endl;
    return false;
  }
  handler_ = std::shared_ptr<LCM2ROSControl_LCMHandler>(new LCM2ROSControl_LCMHandler(*this));

        // Check which joints we have been assigned to
        // If we have joints assigned to just us, claim those, otherwise claim all
  std::vector<std::string> joint_names_;
  if (!controller_nh.getParam("joints", joint_names_))
    ROS_INFO_STREAM("Could not get assigned list of joints, will resume to claim all");

  auto n_joints_ = joint_names_.size();
  bool use_joint_selection = true;
  if (n_joints_ == 0)
    use_joint_selection = false;

        // get joint limits
  for (auto const &joint_name : joint_names_) {
    joint_limits_interface::JointLimits limits;
    if (!getJointLimits(joint_name, controller_nh, limits)){
      ROS_INFO_STREAM("Cannot read joint limits for joint " << joint_name << " from param server");
    } else {
      joint_limits.insert(std::make_pair(joint_name, limits));
      ROS_INFO_STREAM("Joint Position Limits: " << joint_name << "position [" << limits.min_position << 
        "," << limits.max_position << "], effort [" << -limits.max_effort << "," << limits.max_effort << "]");
    }
  }

        // get a pointer to the effort interface
  hardware_interface::EffortJointInterface* effort_hw = robot_hw->get<hardware_interface::EffortJointInterface>();
  if (!effort_hw)
  {
    ROS_ERROR("This controller requires a hardware interface of type hardware_interface::EffortJointInterface.");
    return false;
  }

  effort_hw->clearClaims();
  const std::vector<std::string>& effortNames = effort_hw->getNames();
        // initialize command buffer for each joint we found on the HW

  for (unsigned int i=0; i<effortNames.size(); i++)
  {
    if (use_joint_selection && std::find(joint_names_.begin(), joint_names_.end(), effortNames[i]) == joint_names_.end())
      continue;

    try {
      effortJointHandles[effortNames[i]] = effort_hw->getHandle(effortNames[i]);
      latest_commands[effortNames[i]] = joint_command();
      latest_commands[effortNames[i]].position = 0.0;
      latest_commands[effortNames[i]].velocity = 0.0;
      latest_commands[effortNames[i]].effort = 0.0;
      latest_commands[effortNames[i]].k_q_p = 0.0;
      latest_commands[effortNames[i]].k_q_i = 0.0;
      latest_commands[effortNames[i]].k_qd_p = 0.0;
      latest_commands[effortNames[i]].k_f_p = 0.0;
      latest_commands[effortNames[i]].ff_qd = 0.0;
      latest_commands[effortNames[i]].ff_qd_d = 0.0;
      latest_commands[effortNames[i]].ff_f_d = 0.0;
      latest_commands[effortNames[i]].ff_const = 0.0;
    } catch (const hardware_interface::HardwareInterfaceException& e) {
      ROS_ERROR_STREAM("Could not retrieve handle for " << effortNames[i] << ": " << e.what());
    }
  }

  auto effort_hw_claims = effort_hw->getClaims();
  claimed_resources.insert(effort_hw_claims.begin(), effort_hw_claims.end());
  effort_hw->clearClaims();

        // get a pointer to the position interface
  hardware_interface::PositionJointInterface* position_hw = robot_hw->get<hardware_interface::PositionJointInterface>();
  if (!position_hw)
  {
    ROS_ERROR("This controller requires a hardware interface of type hardware_interface::PositionJointInterface.");
    return false;
  }

  position_hw->clearClaims();
  const std::vector<std::string>& positionNames = position_hw->getNames();
        // initialize command buffer for each joint we found on the HW
  for(unsigned int i=0; i<positionNames.size(); i++)
  {
    if (use_joint_selection && std::find(joint_names_.begin(), joint_names_.end(), positionNames[i]) == joint_names_.end())
      continue;

    try {
      positionJointHandles[positionNames[i]] = position_hw->getHandle(positionNames[i]);
      latest_commands[positionNames[i]] = joint_command();
      latest_commands[positionNames[i]].position = 0.0;
      latest_commands[positionNames[i]].velocity = 0.0;
      latest_commands[positionNames[i]].effort = 0.0;
      latest_commands[positionNames[i]].k_q_p = 0.0;
      latest_commands[positionNames[i]].k_q_i = 0.0;
      latest_commands[positionNames[i]].k_qd_p = 0.0;
      latest_commands[positionNames[i]].k_f_p = 0.0;
      latest_commands[positionNames[i]].ff_qd = 0.0;
      latest_commands[positionNames[i]].ff_qd_d = 0.0;
      latest_commands[positionNames[i]].ff_f_d = 0.0;
      latest_commands[positionNames[i]].ff_const = 0.0;
    } catch (const hardware_interface::HardwareInterfaceException& e) {
      ROS_ERROR_STREAM("Could not retrieve handle for " << positionNames[i] << ": " << e.what());
    }
  }

  auto position_hw_claims = position_hw->getClaims();
  claimed_resources.insert(position_hw_claims.begin(), position_hw_claims.end());
  position_hw->clearClaims();

        // get a pointer to the imu interface
  hardware_interface::ImuSensorInterface* imu_hw = robot_hw->get<hardware_interface::ImuSensorInterface>();
  if (!imu_hw)
  {
    ROS_ERROR("This controller requires a hardware interface of type hardware_interface::ImuSensorInterface.");
    return false;
  }

  imu_hw->clearClaims();
  const std::vector<std::string>& imuNames = imu_hw->getNames();
  for(unsigned int i=0; i<imuNames.size(); i++)
  {
    if (use_joint_selection && std::find(joint_names_.begin(), joint_names_.end(), imuNames[i]) == joint_names_.end())
      continue;

    try {
      imuSensorHandles[imuNames[i]] = imu_hw->getHandle(imuNames[i]);
    } catch (const hardware_interface::HardwareInterfaceException& e) {
      ROS_ERROR_STREAM("Could not retrieve handle for " << imuNames[i] << ": " << e.what());
    }
  }

  auto imu_hw_claims = imu_hw->getClaims();
  claimed_resources.insert(imu_hw_claims.begin(), imu_hw_claims.end());
  imu_hw->clearClaims();

  hardware_interface::ForceTorqueSensorInterface* forceTorque_hw = robot_hw->get<hardware_interface::ForceTorqueSensorInterface>();
  if (!forceTorque_hw)
  {
    ROS_ERROR("This controller requires a hardware interface of type hardware_interface::ForceTorqueSensorInterface.");
    return false;
  }

        // get pointer to forcetorque interface
  forceTorque_hw->clearClaims();
  const std::vector<std::string>& forceTorqueNames = forceTorque_hw->getNames();
  for(unsigned int i=0; i<forceTorqueNames.size(); i++)
  {
    if (use_joint_selection && std::find(joint_names_.begin(), joint_names_.end(), forceTorqueNames[i]) == joint_names_.end())
      continue;

    try {
      forceTorqueHandles[forceTorqueNames[i]] = forceTorque_hw->getHandle(forceTorqueNames[i]);
    } catch (const hardware_interface::HardwareInterfaceException& e) {
      ROS_ERROR_STREAM("Could not retrieve handle for " << forceTorqueNames[i] << ": " << e.what());
    }
  }
  auto forceTorque_hw_claims = forceTorque_hw->getClaims();
  claimed_resources.insert(forceTorque_hw_claims.begin(), forceTorque_hw_claims.end());
  forceTorque_hw->clearClaims();

        // success
  state_ = INITIALIZED;
  ROS_INFO_STREAM("LCM2ROSCONTROL ON with " << claimed_resources.size() << " claimed resources:" << std::endl
    << forceTorque_hw_claims.size() << " force torque" << std::endl
    << imu_hw_claims.size() << " IMUs" << std::endl
    << effort_hw_claims.size() << " effort-controlled joints" << std::endl
    << position_hw_claims.size() << " position-controlled joints" << std::endl);
  return true;
}

void LCM2ROSControl::starting(const ros::Time& time)
{
  last_update = time;
}

void LCM2ROSControl::update(const ros::Time& time, const ros::Duration& period)
{
  handler_->update();
  lcm_->handleTimeout(0);

  double dt = (time - last_update).toSec();
  last_update = time;
  int64_t utime = time.toSec() * 1000000.;

  size_t numberOfJointInterfaces = effortJointHandles.size() + positionJointHandles.size();

      // CORE_ROBOT_STATE
      // push out the joint states for all joints we see advertised
      // and also the commanded torques, for reference
  bot_core::joint_state_t lcm_pose_msg;
  lcm_pose_msg.utime = utime;
  lcm_pose_msg.num_joints = numberOfJointInterfaces;
  lcm_pose_msg.joint_name.assign(numberOfJointInterfaces, "");
  lcm_pose_msg.joint_position.assign(numberOfJointInterfaces, 0.);
  lcm_pose_msg.joint_velocity.assign(numberOfJointInterfaces, 0.);
  lcm_pose_msg.joint_effort.assign(numberOfJointInterfaces, 0.);

      // VAL_COMMAND_FEEDBACK
  bot_core::joint_state_t lcm_commanded_msg;
  lcm_commanded_msg.utime = utime;
  lcm_commanded_msg.num_joints = numberOfJointInterfaces;
  lcm_commanded_msg.joint_name.assign(numberOfJointInterfaces, "");
  lcm_commanded_msg.joint_position.assign(numberOfJointInterfaces, 0.);
  lcm_commanded_msg.joint_velocity.assign(numberOfJointInterfaces, 0.);
  lcm_commanded_msg.joint_effort.assign(numberOfJointInterfaces, 0.);

      // VAL_COMMAND_FEEDBACK_TORQUE
      // TODO: add the position elements here, even though they aren't torques
  bot_core::joint_angles_t lcm_torque_msg;
  lcm_torque_msg.robot_name = "val!";
  lcm_torque_msg.utime = utime;
  lcm_torque_msg.num_joints = effortJointHandles.size();
  lcm_torque_msg.joint_name.assign(effortJointHandles.size(), "");
  lcm_torque_msg.joint_position.assign(effortJointHandles.size(), 0.);

      // EST_ROBOT_STATE
      // need to decide what message we're really using for state. for now,
      // assembling this to make director happy
  bot_core::robot_state_t lcm_state_msg;
  lcm_state_msg.utime = utime;
  lcm_state_msg.num_joints = numberOfJointInterfaces;
  lcm_state_msg.joint_name.assign(numberOfJointInterfaces, "");
  lcm_state_msg.joint_position.assign(numberOfJointInterfaces, 0.);
  lcm_state_msg.joint_velocity.assign(numberOfJointInterfaces, 0.);
  lcm_state_msg.joint_effort.assign(numberOfJointInterfaces, 0.);
  lcm_state_msg.pose.translation.x = 0.0;
  lcm_state_msg.pose.translation.y = 0.0;
  lcm_state_msg.pose.translation.z = 0.0;
  lcm_state_msg.pose.rotation.w = 1.0;
  lcm_state_msg.pose.rotation.x = 0.0;
  lcm_state_msg.pose.rotation.y = 0.0;
  lcm_state_msg.pose.rotation.z = 0.0;
  lcm_state_msg.twist.linear_velocity.x = 0.0;
  lcm_state_msg.twist.linear_velocity.y = 0.0;
  lcm_state_msg.twist.linear_velocity.z = 0.0;
  lcm_state_msg.twist.angular_velocity.x = 0.0;
  lcm_state_msg.twist.angular_velocity.y = 0.0;
  lcm_state_msg.twist.angular_velocity.z = 0.0;


      // Iterate over all effort-controlled joints
  size_t effortJointIndex = 0;
  for(auto iter = effortJointHandles.begin(); iter != effortJointHandles.end(); iter++)
  {
          // see drc_joint_command_t.lcm for explanation of gains and
          // force calculation.
    double q = iter->second.getPosition();
    double qd = iter->second.getVelocity();
    double f = iter->second.getEffort();

    joint_command& command = latest_commands[iter->first];
    double command_effort =
    command.k_q_p * ( command.position - q ) +
    command.k_q_i * ( command.position - q ) * dt +
    command.k_qd_p * ( command.velocity - qd) +
    command.k_f_p * ( command.effort - f) +
    command.ff_qd * ( qd ) +
    command.ff_qd_d * ( command.velocity ) +
    command.ff_f_d * ( command.effort ) +
    command.ff_const;


    auto limits_search = joint_limits.find(iter->first);
    joint_limits_interface::JointLimits limits;
    if (limits_search == joint_limits.end()){
            // defaults
      limits.min_position = DEFAULT_MIN_POSITION;
      limits.max_position = DEFAULT_MAX_POSITION;
      limits.max_effort = DEFAULT_MAX_EFFORT;
    } else {
      limits = limits_search->second;
    }

          // bound the force within our max force limits
    command_effort = clamp(command_effort, -limits.max_effort, limits.max_effort);

           // and ramp down the force to 0 in the 0.1 radians after the joint limit
    double err_beyond_bound = fmax(q - limits.max_position, limits.min_position - q);
    if (err_beyond_bound >= FORCE_CONTROL_ALLOWABLE_POSITION_ERR_BOUND){
      ROS_INFO("Dangerous command modified: joint %s force %f nulled due to joint out of range %f\n", iter->first.c_str(), command_effort, q);
      command_effort = 0.0;
    }
    else if (err_beyond_bound >= 0){              
     ROS_INFO("Dangerous command modified: joint %s force %f scaled due to joint out of range %f\n", iter->first.c_str(), command_effort, q);
            command_effort *= (FORCE_CONTROL_ALLOWABLE_POSITION_ERR_BOUND - err_beyond_bound) / FORCE_CONTROL_ALLOWABLE_POSITION_ERR_BOUND; // start at no scaling, scale down to 0 at ERR_BOUND
          }

          // finally, bound to force to be within epsilon of the currently applied force
          if (fabs(command_effort - f) >= FORCE_CONTROL_MAX_CHANGE)
            ROS_INFO("Dangerous command modified: joint %s force %f out of range of current force %f\n", iter->first.c_str(), command_effort, f);

          if (command_effort > f)
            command_effort = fmin(f + FORCE_CONTROL_MAX_CHANGE, command_effort);
          else
            command_effort = fmax(f - FORCE_CONTROL_MAX_CHANGE, command_effort);

          // only apply this command to the robot if this flag is set to true
          if(applyCommands){
            // apply those forces
            if (fabs(command_effort) < 1000.){
              iter->second.setCommand(command_effort);
            } else {
              ROS_INFO("Dangerous latest_commands for joint %s: somehow commanding %f\n", iter->first.c_str(), command_effort);
              iter->second.setCommand(0.0);
            }
          }


          lcm_pose_msg.joint_name[effortJointIndex] = iter->first;
          lcm_pose_msg.joint_position[effortJointIndex] = q;
          lcm_pose_msg.joint_velocity[effortJointIndex] = qd;
          lcm_pose_msg.joint_effort[effortJointIndex] = iter->second.getEffort(); // measured!

          lcm_state_msg.joint_name[effortJointIndex] = iter->first;
          lcm_state_msg.joint_position[effortJointIndex] = q;
          lcm_state_msg.joint_velocity[effortJointIndex] = qd;
          lcm_state_msg.joint_effort[effortJointIndex] = iter->second.getEffort(); // measured!

          // republish to guarantee sync
          lcm_commanded_msg.joint_name[effortJointIndex] = iter->first;
          lcm_commanded_msg.joint_position[effortJointIndex] = command.position;
          lcm_commanded_msg.joint_velocity[effortJointIndex] = command.velocity;
          lcm_commanded_msg.joint_effort[effortJointIndex] = command.effort;

          lcm_torque_msg.joint_name[effortJointIndex] = iter->first;
          lcm_torque_msg.joint_position[effortJointIndex] = command_effort;

          effortJointIndex++;
        }

      // Iterate over all position-controlled joints
        size_t positionJointIndex = effortJointIndex;
        for (auto iter = positionJointHandles.begin(); iter != positionJointHandles.end(); iter++) {
          double q = iter->second.getPosition();
          double qd = iter->second.getVelocity();

          joint_command& command = latest_commands[iter->first];
          double position_to_go = command.position;

          // clamp to joint limits
          auto limits_search = joint_limits.find(iter->first);
          joint_limits_interface::JointLimits limits;
          if (limits_search == joint_limits.end()){
            // defaults
            limits.min_position = DEFAULT_MIN_POSITION;
            limits.max_position = DEFAULT_MAX_POSITION;
            limits.max_effort = DEFAULT_MAX_EFFORT;
          }
          if (position_to_go > limits.max_position || position_to_go < limits.min_position)
            ROS_INFO("Dangerous command modified: joint %s position %f out of joint limits\n", iter->first.c_str(), position_to_go);

          position_to_go = clamp(position_to_go, limits.min_position, limits.max_position);

          if (applyCommands){
            iter->second.setCommand(position_to_go);
          }

          // TODO: we can't directly iterate like this, better match differently!!

          lcm_pose_msg.joint_name[positionJointIndex] = iter->first;
          lcm_pose_msg.joint_position[positionJointIndex] = q;
          lcm_pose_msg.joint_velocity[positionJointIndex] = qd;

          lcm_state_msg.joint_name[positionJointIndex] = iter->first;
          lcm_state_msg.joint_position[positionJointIndex] = q;
          lcm_state_msg.joint_velocity[positionJointIndex] = qd;

          // republish to guarantee sync
          lcm_commanded_msg.joint_name[positionJointIndex] = iter->first;
          lcm_commanded_msg.joint_position[positionJointIndex] = command.position;
          lcm_commanded_msg.joint_velocity[positionJointIndex] = command.velocity;
          lcm_commanded_msg.joint_effort[positionJointIndex] = command.effort;

          positionJointIndex++;
        }
      
    }

    void LCM2ROSControl::stopping(const ros::Time& time)
    {}

    LCM2ROSControl_LCMHandler::LCM2ROSControl_LCMHandler(LCM2ROSControl& parent) : parent_(parent) {
      lcm_ = std::shared_ptr<lcm::LCM>(new lcm::LCM);
      if (!lcm_->good())
      {
        std::cerr << "ERROR: handler lcm is not good()" << std::endl;
      }
      lcm_->subscribe("ROBOT_COMMAND", &LCM2ROSControl_LCMHandler::jointCommandHandler, this);
    }
    LCM2ROSControl_LCMHandler::~LCM2ROSControl_LCMHandler() {}

    void LCM2ROSControl_LCMHandler::jointCommandHandler(const lcm::ReceiveBuffer* rbuf, const std::string &channel,
     const bot_core::atlas_command_t* msg) {
      // TODO: zero non-mentioned joints for safety?

      for (unsigned int i = 0; i < msg->num_joints; ++i) {
        // ROS_WARN("Joint %s ", msg->joint_names[i].c_str());
        auto search = parent_.latest_commands.find(msg->joint_names[i]);
        if (search != parent_.latest_commands.end()) {
          joint_command& command = parent_.latest_commands[msg->joint_names[i]];
          command.position = msg->position[i];
          command.velocity = msg->velocity[i];
          command.effort = msg->effort[i];
          command.k_q_p = msg->k_q_p[i];
          command.k_q_i = msg->k_q_i[i];
          command.k_qd_p = msg->k_qd_p[i];
          command.k_f_p = msg->k_f_p[i];
          command.ff_qd = msg->ff_qd[i];
          command.ff_qd_d = msg->ff_qd_d[i];
          command.ff_f_d = msg->ff_f_d[i];
          command.ff_const = msg->ff_const[i];
        } else {
          // ROS_WARN("had no match.");
        }
      }
    }
    void LCM2ROSControl_LCMHandler::update(){
      lcm_->handleTimeout(0);
    }
  }

  PLUGINLIB_EXPORT_CLASS(valkyrie_translator::LCM2ROSControl, controller_interface::ControllerBase)
