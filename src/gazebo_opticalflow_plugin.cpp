/*
 * Copyright (C) 2012-2015 Open Source Robotics Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
*/
#ifdef _WIN32
  // Ensure that Winsock2.h is included before Windows.h, which can get
  // pulled in by anybody (e.g., Boost).
#include <Winsock2.h>
#endif

#include "gazebo/sensors/DepthCameraSensor.hh"
#include "gazebo_opticalflow_plugin.h"

#include <highgui.h>
#include <math.h>
#include <string>
#include <iostream>
#include <boost/algorithm/string.hpp>

using namespace cv;
using namespace std;

using namespace gazebo;
GZ_REGISTER_SENSOR_PLUGIN(OpticalFlowPlugin)

/////////////////////////////////////////////////
OpticalFlowPlugin::OpticalFlowPlugin()
: SensorPlugin(), width(0), height(0), depth(0), timer_()
{

}

/////////////////////////////////////////////////
OpticalFlowPlugin::~OpticalFlowPlugin()
{
  this->parentSensor.reset();
  this->camera.reset();
}

/////////////////////////////////////////////////
void OpticalFlowPlugin::Load(sensors::SensorPtr _sensor, sdf::ElementPtr _sdf)
{
  if (!_sensor)
    gzerr << "Invalid sensor pointer.\n";

  this->parentSensor =
#if GAZEBO_MAJOR_VERSION >= 7
    std::dynamic_pointer_cast<sensors::CameraSensor>(_sensor);
#else
    boost::dynamic_pointer_cast<sensors::CameraSensor>(_sensor);
#endif

  if (!this->parentSensor)
  {
    gzerr << "OpticalFlowPlugin requires a CameraSensor.\n";
#if GAZEBO_MAJOR_VERSION >= 7
    if (std::dynamic_pointer_cast<sensors::DepthCameraSensor>(_sensor))
#else
    if (boost::dynamic_pointer_cast<sensors::DepthCameraSensor>(_sensor))
#endif
      gzmsg << "It is a depth camera sensor\n";
  }

  if (!this->parentSensor)
  {
    gzerr << "OpticalFlowPlugin not attached to a camera sensor\n";
    return;
  }

#if GAZEBO_MAJOR_VERSION >= 7
this->camera = this->parentSensor->Camera();
  this->width = this->camera->ImageWidth();
  this->height = this->camera->ImageHeight();
  this->depth = this->camera->ImageDepth();
  this->format = this->camera->ImageFormat();
  hfov_ = float(this->camera->HFOV().Radian());
  first_frame_time_ = this->camera->LastRenderWallTime().Double();
  const string scopedName = _sensor->ParentName();
#else
  this->camera = this->parentSensor->GetCamera();
  this->width = this->camera->GetImageWidth();
  this->height = this->camera->GetImageHeight();
  this->depth = this->camera->GetImageDepth();
  this->format = this->camera->GetImageFormat();
  hfov_ = float(this->camera->GetHFOV().Radian());
  first_frame_time_ = this->camera->GetLastRenderWallTime().Double();
  const string scopedName = _sensor->GetParentName();
#endif

  focal_length_ = (this->width/2)/tan(hfov_/2);

  if (this->width != 64 || this->height != 64) {
    gzerr << "[gazebo_optical_flow_plugin] Incorrect image size, must by 64 x 64.\n";
  }

  if (_sdf->HasElement("robotNamespace"))
    namespace_ = _sdf->GetElement("robotNamespace")->Get<std::string>();
  else
    gzwarn << "[gazebo_optical_flow_plugin] Please specify a robotNamespace.\n";

  if (_sdf->HasElement("outputRate")) {
    output_rate_ = _sdf->GetElement("outputRate")->Get<int>();
  } else {
    output_rate_ = DEFAULT_RATE;
    gzwarn << "[gazebo_optical_flow_plugin] Using default output rate " << output_rate_ << ".";
  }

  node_handle_ = transport::NodePtr(new transport::Node());
  node_handle_->Init(namespace_);

  string topicName = "~/" + scopedName + "/opticalFlow";
  boost::replace_all(topicName, "::", "/");

  opticalFlow_pub_ = node_handle_->Advertise<sensor_msgs::msgs::OpticalFlow>(topicName, 10);

  this->newFrameConnection = this->camera->ConnectNewImageFrame(
      boost::bind(&OpticalFlowPlugin::OnNewFrame, this, _1, this->width, this->height, this->depth, this->format));

  this->parentSensor->SetActive(true);

  //init flow
  optical_flow_ = new OpticalFlowOpenCV(focal_length_, focal_length_, output_rate_);
  // _optical_flow = new OpticalFlowPX4(focal_length_, focal_length_, output_rate_, this->width);

}

/////////////////////////////////////////////////
void OpticalFlowPlugin::OnNewFrame(const unsigned char * _image,
                              unsigned int _width,
                              unsigned int _height,
                              unsigned int _depth,
                              const std::string &_format)
{

  //get data depending on gazebo version
  #if GAZEBO_MAJOR_VERSION >= 7
    _image = this->camera->ImageData(0);
    double frame_time = this->camera->LastRenderWallTime().Double();
  #else
    _image = this->camera->GetImageData(0);
    double frame_time = this->camera->GetLastRenderWallTime().Double();
  #endif

  frame_time_us_ = (frame_time - first_frame_time_) * 1e6; //since start

  timer_.stop();

  float flow_x_ang = 0.0f;
  float flow_y_ang = 0.0f;
  //calculate angular flow
  int quality = optical_flow_->calcFlow((uchar*)_image, frame_time_us_, dt_us_, flow_x_ang, flow_y_ang);

  if (quality >= 0) { // calcFlow(...) returns -1 if data should not be published yet -> output_rate
    //prepare optical flow message
    opticalFlow_message.set_time_usec(0);//will be filled in simulator_mavlink.cpp
    opticalFlow_message.set_sensor_id(2.0);
    opticalFlow_message.set_integration_time_us(quality ? dt_us_ : 0);
    opticalFlow_message.set_integrated_x(quality ? flow_x_ang : 0.0f);
    opticalFlow_message.set_integrated_y(quality ? flow_y_ang : 0.0f);
    opticalFlow_message.set_integrated_xgyro(0.0f); //get real values in gazebo_mavlink_interface.cpp
    opticalFlow_message.set_integrated_ygyro(0.0f); //get real values in gazebo_mavlink_interface.cpp
    opticalFlow_message.set_integrated_zgyro(0.0f); //get real values in gazebo_mavlink_interface.cpp
    opticalFlow_message.set_temperature(20.0f);
    opticalFlow_message.set_quality(quality);
    opticalFlow_message.set_time_delta_distance_us(0);
    opticalFlow_message.set_distance(0.0f); //get real values in gazebo_mavlink_interface.cpp
    //send message
    opticalFlow_pub_->Publish(opticalFlow_message);
    timer_.start();
  }
}

/* vim: set et fenc=utf-8 ff=unix sts=0 sw=2 ts=2 : */
