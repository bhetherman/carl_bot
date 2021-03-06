#include <carl_interactive_manipulation/carl_interactive_manipulation.h>

using namespace std;

CarlInteractiveManipulation::CarlInteractiveManipulation() :
    acGripper("jaco_arm/manipulation/gripper", true),
    acArm("carl_moveit_wrapper/common_actions/arm_action", true),
    acPickup("carl_moveit_wrapper/common_actions/pickup", true)
{
  joints.resize(6);

  //read parameters
  ros::NodeHandle pnh("~");
  usingPickup = false;
  pnh.getParam("using_pickup", usingPickup);

  //messages
  cartesianCmd = n.advertise<wpi_jaco_msgs::CartesianCommand>("jaco_arm/cartesian_cmd", 1);
  segmentedObjectsPublisher = n.advertise<rail_manipulation_msgs::SegmentedObjectList>("rail_segmentation/segmented_objects", 1);
  safetyErrorPublisher = n.advertise<carl_safety::Error>("carl_safety/error", 1);
  jointStateSubscriber = n.subscribe("jaco_arm/joint_states", 1, &CarlInteractiveManipulation::updateJoints, this);
  recognizedObjectsSubscriber = n.subscribe("/object_recognition_listener/recognized_objects", 1, &CarlInteractiveManipulation::segmentedObjectsCallback, this);

  //services
  armCartesianPositionClient = n.serviceClient<wpi_jaco_msgs::GetCartesianPosition>("jaco_arm/get_cartesian_position");
  armEStopClient = n.serviceClient<wpi_jaco_msgs::EStop>("jaco_arm/software_estop");
  eraseTrajectoriesClient = n.serviceClient<std_srvs::Empty>("jaco_arm/erase_trajectories");
  jacoFkClient = n.serviceClient<wpi_jaco_msgs::JacoFK>("jaco_arm/kinematics/fk");
  qeClient = n.serviceClient<wpi_jaco_msgs::QuaternionToEuler>("jaco_conversions/quaternion_to_euler");
  //pickupSegmentedClient = n.serviceClient<rail_pick_and_place_msgs::PickupSegmentedObject>("rail_pick_and_place/pickup_segmented_object");
  removeObjectClient = n.serviceClient<rail_segmentation::RemoveObject>("rail_segmentation/remove_object");
  detachObjectsClient = n.serviceClient<std_srvs::Empty>("carl_moveit_wrapper/detach_objects");

  //actionlib
  ROS_INFO("Waiting for grasp action servers...");
  acGripper.waitForServer();
  acArm.waitForServer();
  ROS_INFO("Finished waiting for action servers");

  markerPose.resize(6);
  lockPose = false;
  movingArm = false;
  disableArmMarkerCommands = false;

  imServer.reset(
      new interactive_markers::InteractiveMarkerServer("carl_interactive_manipulation", "carl_markers", false));

  ros::Duration(0.1).sleep();

  makeHandMarker();

  //setup object menu
  if (usingPickup)
    objectMenuHandler.insert("Pickup", boost::bind(&CarlInteractiveManipulation::processPickupMarkerFeedback, this, _1));
  objectMenuHandler.insert("Remove", boost::bind(&CarlInteractiveManipulation::processRemoveMarkerFeedback, this, _1));

  imServer->applyChanges();
}

void CarlInteractiveManipulation::updateJoints(const sensor_msgs::JointState::ConstPtr& msg)
{
  for (unsigned int i = 0; i < 6; i++)
  {
    joints.at(i) = msg->position.at(i);
  }

  //perform safety check if arm is moving due to interactive marker Cartesian control
  if (movingArm)
  {
    if (fabs(msg->effort[0]) >= J1_THRESHOLD || fabs(msg->effort[1] >= J2_THRESHOLD) || fabs(msg->effort[2] >= J3_THRESHOLD)
        || fabs(msg->effort[3]) >= J4_THRESHOLD || fabs(msg->effort[4] >= J5_THRESHOLD) || fabs(msg->effort[5] >= J6_THRESHOLD)
        || fabs(msg->effort[6]) >= F1_THRESHOLD || fabs(msg->effort[7] >= F2_THRESHOLD) || fabs(msg->effort[8] >= F3_THRESHOLD))
    {
      ROS_INFO("Arm is moving dangerously, attempty recovery...");
      armCollisionRecovery();
    }
  }
}

void CarlInteractiveManipulation::segmentedObjectsCallback(
    const rail_manipulation_msgs::SegmentedObjectList::ConstPtr& objectList)
{
  //store list of objects
  segmentedObjectList = *objectList;

  ROS_INFO("Received new segmented point clouds");
  clearSegmentedObjects();
  recognizedMenuHandlers.clear();
  recognizedMenuHandlers.resize(objectList->objects.size());
  for (unsigned int i = 0; i < objectList->objects.size(); i++)
  {
    visualization_msgs::InteractiveMarker objectMarker;
    objectMarker.header = objectList->objects[i].marker.header;

    objectMarker.pose.position.x = 0.0;
    objectMarker.pose.position.y = 0.0;
    objectMarker.pose.position.z = 0.0;
    objectMarker.pose.orientation.x = 0.0;
    objectMarker.pose.orientation.y = 0.0;
    objectMarker.pose.orientation.z = 0.0;
    objectMarker.pose.orientation.w = 1.0;

    stringstream ss;
    ss.str("");
    ss << "object" << i;
    objectMarker.name = ss.str();

    visualization_msgs::InteractiveMarkerControl objectControl;
    ss << "control";
    objectControl.name = ss.str();
    objectControl.interaction_mode = visualization_msgs::InteractiveMarkerControl::BUTTON;
    //objectControl.interaction_mode = visualization_msgs::InteractiveMarkerControl::MENU;
    objectControl.always_visible = true;
    objectControl.markers.resize(1);
    //objectControl.markers[0] = cloudMarker;
    objectControl.markers[0] = objectList->objects[i].marker;
    objectMarker.controls.push_back(objectControl);

    //object label
    visualization_msgs::InteractiveMarkerControl objectLabelControl;
    stringstream namestream;
    namestream.str("");
    namestream << "object" << i << "_label";
    objectLabelControl.name = namestream.str();
    objectLabelControl.interaction_mode = visualization_msgs::InteractiveMarkerControl::NONE;
    objectLabelControl.always_visible = true;
    visualization_msgs::Marker objectLabel;
    objectLabel.header = objectList->objects[i].point_cloud.header;
    objectLabel.type = visualization_msgs::Marker::TEXT_VIEW_FACING;
    objectLabel.pose.position.x = objectList->objects[i].centroid.x;
    objectLabel.pose.position.y = objectList->objects[i].centroid.y;
    objectLabel.pose.position.z = objectList->objects[i].centroid.z + .1;
    objectLabel.scale.x = .1;
    objectLabel.scale.y = .1;
    objectLabel.scale.z = .1;
    objectLabel.color.r = 1.0;
    objectLabel.color.g = 1.0;
    objectLabel.color.b = 1.0;
    objectLabel.color.a = 1.0;
    objectLabel.text = objectList->objects[i].name;
    objectLabelControl.markers.resize(1);
    objectLabelControl.markers[0] = objectLabel;
    objectMarker.controls.push_back(objectLabelControl);

    //pickup menu
    visualization_msgs::InteractiveMarkerControl objectMenuControl;
    objectMenuControl.interaction_mode = visualization_msgs::InteractiveMarkerControl::MENU;
    ss << "_menu";
    objectMenuControl.name = ss.str();
    objectMarker.controls.push_back(objectMenuControl);

    imServer->insert(objectMarker);

    if (objectList->objects[i].recognized)
    {
      if (usingPickup)
      {
        stringstream ss2;
        ss2.str("");
        ss2 << "Pickup " << objectList->objects[i].name;
        recognizedMenuHandlers[i].insert(ss2.str(), boost::bind(&CarlInteractiveManipulation::processPickupMarkerFeedback, this, _1));
      }
      recognizedMenuHandlers[i].insert("Remove", boost::bind(&CarlInteractiveManipulation::processRemoveMarkerFeedback, this, _1));
      recognizedMenuHandlers[i].apply(*imServer, objectMarker.name);
    }
    else
    {
      objectMenuHandler.apply(*imServer, objectMarker.name);
    }

    segmentedObjects.push_back(objectMarker);
  }

  imServer->applyChanges();

  ROS_INFO("Point cloud markers created");
}

void CarlInteractiveManipulation::clearSegmentedObjects()
{
  for (unsigned int i = 0; i < segmentedObjects.size(); i++)
  {
    stringstream ss;
    ss.str("");
    ss << "object" << i;
    imServer->erase(ss.str());
  }
  segmentedObjects.clear();

  imServer->applyChanges();
}

void CarlInteractiveManipulation::makeHandMarker()
{
  visualization_msgs::InteractiveMarker iMarker;
  iMarker.header.frame_id = "jaco_link_base";

  //initialize position to the jaco arm's current position
  wpi_jaco_msgs::JacoFK fkSrv;
  for (unsigned int i = 0; i < 6; i++)
  {
    fkSrv.request.joints.push_back(joints.at(i));
  }
  if (jacoFkClient.call(fkSrv))
  {
    iMarker.pose = fkSrv.response.handPose.pose;
  }
  else
  {
    iMarker.pose.position.x = 0.0;
    iMarker.pose.position.y = 0.0;
    iMarker.pose.position.z = 0.0;
    iMarker.pose.orientation.x = 0.0;
    iMarker.pose.orientation.y = 0.0;
    iMarker.pose.orientation.z = 0.0;
    iMarker.pose.orientation.w = 1.0;
  }
  iMarker.scale = .2;

  iMarker.name = "jaco_hand_marker";
  iMarker.description = "JACO Hand Control";

  //make a sphere control to represent the end effector position
  visualization_msgs::Marker sphereMarker;
  sphereMarker.type = visualization_msgs::Marker::SPHERE;
  sphereMarker.scale.x = iMarker.scale * 1;
  sphereMarker.scale.y = iMarker.scale * 1;
  sphereMarker.scale.z = iMarker.scale * 1;
  sphereMarker.color.r = .5;
  sphereMarker.color.g = .5;
  sphereMarker.color.b = .5;
  sphereMarker.color.a = 0.0;
  visualization_msgs::InteractiveMarkerControl sphereControl;
  sphereControl.markers.push_back(sphereMarker);
  sphereControl.interaction_mode = visualization_msgs::InteractiveMarkerControl::BUTTON;
  sphereControl.name = "jaco_hand_origin_marker";
  iMarker.controls.push_back(sphereControl);

  //add 6-DOF controls
  visualization_msgs::InteractiveMarkerControl control;

  control.orientation.w = 1;
  control.orientation.x = 1;
  control.orientation.y = 0;
  control.orientation.z = 0;
  control.name = "rotate_x";
  control.interaction_mode = visualization_msgs::InteractiveMarkerControl::ROTATE_AXIS;
  iMarker.controls.push_back(control);
  control.name = "move_x";
  control.interaction_mode = visualization_msgs::InteractiveMarkerControl::MOVE_AXIS;
  iMarker.controls.push_back(control);

  control.orientation.w = 1;
  control.orientation.x = 0;
  control.orientation.y = 1;
  control.orientation.z = 0;
  control.name = "rotate_y";
  control.interaction_mode = visualization_msgs::InteractiveMarkerControl::ROTATE_AXIS;
  iMarker.controls.push_back(control);
  control.name = "move_y";
  control.interaction_mode = visualization_msgs::InteractiveMarkerControl::MOVE_AXIS;
  iMarker.controls.push_back(control);

  control.orientation.w = 1;
  control.orientation.x = 0;
  control.orientation.y = 0;
  control.orientation.z = 1;
  control.name = "rotate_z";
  control.interaction_mode = visualization_msgs::InteractiveMarkerControl::ROTATE_AXIS;
  iMarker.controls.push_back(control);
  control.name = "move_z";
  control.interaction_mode = visualization_msgs::InteractiveMarkerControl::MOVE_AXIS;
  iMarker.controls.push_back(control);

  //menu
  interactive_markers::MenuHandler::EntryHandle fingersSubMenuHandle = menuHandler.insert("Gripper");
  menuHandler.insert(fingersSubMenuHandle, "Close",
                     boost::bind(&CarlInteractiveManipulation::processHandMarkerFeedback, this, _1));
  menuHandler.insert(fingersSubMenuHandle, "Open",
                     boost::bind(&CarlInteractiveManipulation::processHandMarkerFeedback, this, _1));
  menuHandler.insert("Ready Arm", boost::bind(&CarlInteractiveManipulation::processHandMarkerFeedback, this, _1));
  menuHandler.insert("Retract Arm", boost::bind(&CarlInteractiveManipulation::processHandMarkerFeedback, this, _1));

  visualization_msgs::InteractiveMarkerControl menuControl;
  menuControl.interaction_mode = visualization_msgs::InteractiveMarkerControl::MENU;
  menuControl.name = "jaco_hand_menu";
  iMarker.controls.push_back(menuControl);

  imServer->insert(iMarker);
  imServer->setCallback(iMarker.name, boost::bind(&CarlInteractiveManipulation::processHandMarkerFeedback, this, _1));

  menuHandler.apply(*imServer, iMarker.name);
}

//void CarlInteractiveManipulation::processRecognizeMarkerFeedback(
//    const visualization_msgs::InteractiveMarkerFeedbackConstPtr &feedback)
//{
//  if (feedback->event_type == visualization_msgs::InteractiveMarkerFeedback::MENU_SELECT)
//  {
//    int objectIndex = atoi(feedback->marker_name.substr(6).c_str());
//    rail_manipulation_msgs::RecognizeGoal goal;
//    goal.index = objectIndex;
//    acRecognize.sendGoal(goal);
//    acRecognize.waitForResult(ros::Duration(10.0));
//  }
//}

void CarlInteractiveManipulation::processPickupMarkerFeedback(
    const visualization_msgs::InteractiveMarkerFeedbackConstPtr &feedback)
{
  if (feedback->event_type == visualization_msgs::InteractiveMarkerFeedback::MENU_SELECT)
  {
    carl_moveit::PickupGoal pickupGoal;
    pickupGoal.lift = true;
    pickupGoal.verify = false;
    int objectIndex = atoi(feedback->marker_name.substr(6).c_str());
    for (unsigned int i = 0; i < segmentedObjectList.objects[objectIndex].grasps.size(); i ++)
    {
      ROS_INFO("ATTEMPTING PICKUP WITH GRASP %d", i);
      pickupGoal.pose = segmentedObjectList.objects[objectIndex].grasps[i];
      acPickup.sendGoal(pickupGoal);
      acPickup.waitForResult(ros::Duration(30.0));

      carl_moveit::PickupResultConstPtr pickupResult = acPickup.getResult();
      if (!pickupResult->success)
      {
	ROS_INFO("PICKUP FAILED, moving on to a new grasp...");
        continue;
      }

      ROS_INFO("PICKUP SUCCEEDED!");

      removeObjectMarker(objectIndex);
      break;
    }
    ROS_INFO("FINISHED ATTEMPTING PICKUPS");

    imServer->applyChanges();
  }
}

void CarlInteractiveManipulation::processRemoveMarkerFeedback(const visualization_msgs::InteractiveMarkerFeedbackConstPtr &feedback)
{
  if (feedback->event_type == visualization_msgs::InteractiveMarkerFeedback::MENU_SELECT)
  {
    if (!removeObjectMarker(atoi(feedback->marker_name.substr(6).c_str())))
      return;

    imServer->applyChanges();
  }
}

bool CarlInteractiveManipulation::removeObjectMarker(int index)
{
  rail_segmentation::RemoveObject::Request req;
  rail_segmentation::RemoveObject::Response res;
  req.index = index;
  if (!removeObjectClient.call(req, res))
  {
    ROS_INFO("Could not call remove object service.");
    return false;
  }
  return true;
}

void CarlInteractiveManipulation::processHandMarkerFeedback(
    const visualization_msgs::InteractiveMarkerFeedbackConstPtr &feedback)
{
  switch (feedback->event_type)
  {
  //Send a stop command so that when the marker is released the arm stops moving
  case visualization_msgs::InteractiveMarkerFeedback::BUTTON_CLICK:
    if (feedback->marker_name.compare("jaco_hand_marker") == 0)
    {
      lockPose = true;
      movingArm = false;
      if (disableArmMarkerCommands)
        disableArmMarkerCommands = false;
      sendStopCommand();
    }
    break;

    //Menu actions
  case visualization_msgs::InteractiveMarkerFeedback::MENU_SELECT:
    if (feedback->marker_name.compare("jaco_hand_marker") == 0)
    {
      if (feedback->menu_entry_id == 2)	//grasp requested
      {
        rail_manipulation_msgs::GripperGoal gripperGoal;
        gripperGoal.close = true;
        acGripper.sendGoal(gripperGoal);
      }
      else if (feedback->menu_entry_id == 3)	//release requested
      {
        rail_manipulation_msgs::GripperGoal gripperGoal;
        gripperGoal.close = false;
        acGripper.sendGoal(gripperGoal);

        std_srvs::Empty emptySrv;
        if (!detachObjectsClient.call(emptySrv))
        {
          ROS_INFO("Couldn't call detach objects service.");
        }
      }
      else if (feedback->menu_entry_id == 4)  //home requested
      {
        acGripper.cancelAllGoals();
        carl_moveit::ArmGoal homeGoal;
        homeGoal.action = carl_moveit::ArmGoal::READY;
        acArm.sendGoal(homeGoal);
        acArm.waitForResult(ros::Duration(10.0));
      }
      else if (feedback->menu_entry_id == 5)
      {
        acGripper.cancelAllGoals();
        carl_moveit::ArmGoal homeGoal;
        homeGoal.action = carl_moveit::ArmGoal::RETRACT;
        acArm.sendGoal(homeGoal);
        acArm.waitForResult(ros::Duration(15.0));
      }
    }
    break;

    //Send movement commands to the arm to follow the pose marker
  case visualization_msgs::InteractiveMarkerFeedback::POSE_UPDATE:
    if (feedback->marker_name.compare("jaco_hand_marker") == 0
        && feedback->control_name.compare("jaco_hand_origin_marker") != 0)
    {
      if (!(lockPose || disableArmMarkerCommands))
      {
        movingArm = true;

        acGripper.cancelAllGoals();

        //convert pose for compatibility with JACO API
        wpi_jaco_msgs::QuaternionToEuler qeSrv;
        qeSrv.request.orientation = feedback->pose.orientation;
        if (qeClient.call(qeSrv))
        {
          wpi_jaco_msgs::CartesianCommand cmd;
          cmd.position = true;
          cmd.armCommand = true;
          cmd.fingerCommand = false;
          cmd.repeat = false;
          cmd.arm.linear.x = feedback->pose.position.x;
          cmd.arm.linear.y = feedback->pose.position.y;
          cmd.arm.linear.z = feedback->pose.position.z;
          cmd.arm.angular.x = qeSrv.response.roll;
          cmd.arm.angular.y = qeSrv.response.pitch;
          cmd.arm.angular.z = qeSrv.response.yaw;

          cartesianCmd.publish(cmd);

          markerPose[0] = feedback->pose.position.x;
          markerPose[1] = feedback->pose.position.y;
          markerPose[2] = feedback->pose.position.z;
          markerPose[3] = qeSrv.response.roll;
          markerPose[4] = qeSrv.response.pitch;
          markerPose[5] = qeSrv.response.yaw;
        }
        else
          ROS_INFO("Quaternion to Euler conversion service failed, could not send pose update");
      }
    }
    break;

    //Mouse down events
  case visualization_msgs::InteractiveMarkerFeedback::MOUSE_DOWN:
    lockPose = false;
    break;

    //As with mouse clicked, send a stop command when the mouse is released on the marker
  case visualization_msgs::InteractiveMarkerFeedback::MOUSE_UP:
    if (feedback->marker_name.compare("jaco_hand_marker") == 0)
    {
      lockPose = true;
      movingArm = false;
      if (disableArmMarkerCommands)
        disableArmMarkerCommands = false;
      sendStopCommand();
    }
    break;
  }

  //Update interactive marker server
  imServer->applyChanges();
}

void CarlInteractiveManipulation::sendStopCommand()
{
  wpi_jaco_msgs::CartesianCommand cmd;
  cmd.position = false;
  cmd.armCommand = true;
  cmd.fingerCommand = false;
  cmd.repeat = true;
  cmd.arm.linear.x = 0.0;
  cmd.arm.linear.y = 0.0;
  cmd.arm.linear.z = 0.0;
  cmd.arm.angular.x = 0.0;
  cmd.arm.angular.y = 0.0;
  cmd.arm.angular.z = 0.0;
  cartesianCmd.publish(cmd);

  std_srvs::Empty srv;
  if (!eraseTrajectoriesClient.call(srv))
  {
    ROS_INFO("Could not call erase trajectories service...");
  }
}

void CarlInteractiveManipulation::updateMarkerPosition()
{
  wpi_jaco_msgs::JacoFK fkSrv;
  for (unsigned int i = 0; i < 6; i++)
  {
    fkSrv.request.joints.push_back(joints.at(i));
  }

  if (jacoFkClient.call(fkSrv))
  {
    imServer->setPose("jaco_hand_marker", fkSrv.response.handPose.pose);
    imServer->applyChanges();
  }
  else
  {
    ROS_INFO("Failed to call forward kinematics service");
  }
}

void CarlInteractiveManipulation::armCollisionRecovery()
{
  //check if recovery is already in progress
  if (disableArmMarkerCommands)
    return;

  wpi_jaco_msgs::GetCartesianPosition posSrv;
  if (!armCartesianPositionClient.call(posSrv))
  {
    ROS_INFO("Could not call arm Cartesian position service.");
    return;
  }
  wpi_jaco_msgs::CartesianCommand cmd;
  cmd.armCommand = true;
  cmd.fingerCommand = false;
  cmd.position = false;
  cmd.repeat = true;
  cmd.arm.linear.x = 0;
  cmd.arm.linear.y = 0;
  cmd.arm.linear.z = 0;
  cmd.arm.angular.x = 0;
  cmd.arm.angular.y = 0;
  cmd.arm.angular.z = 0;

  vector<float> error;
  error.resize(3);
  error[0] = posSrv.response.pos.linear.x - markerPose[0];
  error[1] = posSrv.response.pos.linear.y - markerPose[1];
  error[2] = posSrv.response.pos.linear.z - markerPose[2];
  /* Currently using translation only, as the rotations don't seem to move the arm into very dangerous positions
  error[3] = posSrv.response.pos.angular.x - markerPose[3];
  error[4] = posSrv.response.pos.angular.y - markerPose[4];
  error[5] = posSrv.response.pos.angular.z - markerPose[5];
  for (unsigned int i = 3; i <= 5; i ++)
  {
    if (error[i] > M_PI)
      error[i] -= M_PI;
    else if (error[i] < -M_PI)
      error[i] += M_PI;
  }
  */
  float maxError;
  if (fabs(error[0]) > fabs(error[1]) && fabs(error[0]) > fabs(error[2]))
  {
    if (error[0] < 0)
      cmd.arm.linear.x = -.175;
    else
      cmd.arm.linear.x = .175;

    maxError = error[0];
  }
  else if (fabs(error[1]) > fabs(error[0]) && fabs(error[1]) > fabs(error[2]))
  {
    if (error[1] < 0)
      cmd.arm.linear.y = -.175;
    else
      cmd.arm.linear.y = .175;
    maxError = error[1];
  }
  else
  {
    if (error[2] < 0)
      cmd.arm.linear.z = -.175;
    else
      cmd.arm.linear.z = .175;
    maxError = error[2];
  }

  //ignore if error is less than 1 cm to prevent accidental clicks from starting a recovery behavior
  if (fabs(maxError) < .01)
  {
    return;
  }

  disableArmMarkerCommands = true;

  //feedback
  carl_safety::Error armCollisionError;
  armCollisionError.message = "Arm in dangerous collision, moving to a safer position...";
  armCollisionError.severity = 1;
  armCollisionError.resolved = false;
  safetyErrorPublisher.publish(armCollisionError);

  wpi_jaco_msgs::EStop eStopSrv;
  eStopSrv.request.enableEStop = true;
  if (!armEStopClient.call(eStopSrv))
  {
    ROS_INFO("Could not call arm EStop service.");
    return;
  }

  eStopSrv.request.enableEStop = false;
  if (!armEStopClient.call(eStopSrv))
  {
    ROS_INFO("Could not call arm EStop service.");
    return;
  }

  ros::Rate loopRate(60);
  for (unsigned int i = 0; i < 60; i ++)
  {
    cartesianCmd.publish(cmd);
    loopRate.sleep();
  }

  //feedback
  carl_safety::Error armCollisionErrorResolved;
  armCollisionError.message = "Arm recovered.";
  armCollisionError.severity = 1;
  armCollisionError.resolved = true;
  safetyErrorPublisher.publish(armCollisionErrorResolved);
}

int main(int argc, char **argv)
{
  ros::init(argc, argv, "jaco_interactive_manipulation");

  CarlInteractiveManipulation cim;

  ros::Rate loop_rate(30);
  while (ros::ok())
  {
    cim.updateMarkerPosition();
    ros::spinOnce();
    loop_rate.sleep();
  }
}

