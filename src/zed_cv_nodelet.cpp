#include "ros/ros.h"
#include <pluginlib/class_list_macros.h>
#include <nodelet/nodelet.h>

#include <image_transport/image_transport.h> // ROS library that allows sending compressed images
#include <camera_info_manager/camera_info_manager.h> // ROS library that publishes CameraInfo topics
#include <sensor_msgs/CameraInfo.h> // ROS message header for CameraInfo
#include <sensor_msgs/image_encodings.h>

#include <diagnostic_updater/diagnostic_updater.h> // Headers for publishing diagnostic messages.
#include <diagnostic_updater/publisher.h>

#include <boost/thread.hpp> // Needed for the nodelet to launch the reading thread.

#include <dynamic_reconfigure/server.h> // Needed for the dynamic_reconfigure gui service to run
#include <zed_cv_driver/ZedConfig.h>

#include <opencv2/opencv.hpp>
#include <cv_bridge/cv_bridge.h>
#include <opencv2/highgui/highgui.hpp>

#include <boost/format.hpp>

#ifndef USE_ZED_SDK
#define USE_ZED_SDK 0
#endif

#if USE_ZED_SDK
#include <zed/Camera.hpp>
#endif

using boost::format;


namespace zed_cv_driver
{
    namespace enc = sensor_msgs::image_encodings;

/**
 * @brief Nodelet version of cv_camera.
 */
class ZedCvNodelet : public nodelet::Nodelet
{
 public:
  ZedCvNodelet() : serial("0"), fw_version(0) {}
  ~ZedCvNodelet()
  {
      pubThread_->interrupt();
      pubThread_->join();
      cleanUp();
  }

 private:

     void paramCallback(zed_cv_driver::ZedConfig &config, uint32_t level)
        {
/*
brightness (int)    : min=0 max=8 step=1 default=4 value=4
contrast (int)    : min=0 max=8 step=1 default=4 value=4
saturation (int)    : min=0 max=8 step=1 default=5 value=6
hue (int)    : min=0 max=11 step=1 default=0 value=0
white_balance_temperature_auto (bool)   : default=1 value=1
gain (int)    : min=0 max=8 step=1 default=4 value=4
power_line_frequency (menu)   : min=0 max=2 default=1 value=1
white_balance_temperature (int)    : min=2800 max=6500 step=100 default=4600 value=4600 flags=inactive
sharpness (int)    : min=0 max=8 step=1 default=4 value=4
exposure_auto (menu)   : min=0 max=3 default=0 value=0
exposure_absolute (int)    : min=5000 max=80000 step=5000 default=60000 value=60000 flags=inactive
exposure_auto_priority (bool)   : default=0 value=0
*/
            config_ = config;
            do_reconfigure_ = true;

        }

  void reconfigure() {
      int w, h;

      NODELET_INFO("Setting resolution: %s", resolution.c_str());

      if(resolution == "2k") {
          w = 4416;
          h = 1242;
      } else if(resolution == "1080p") {
          w = 3840;
          h = 1080;
      } else if(resolution == "vga") {
          w = 1280;
          h = 480;
      } else {
          w = 2560;
          h = 720;
      }

      cap_.set(CV_CAP_PROP_FRAME_WIDTH, w);
      cap_.set(CV_CAP_PROP_FRAME_HEIGHT, h);

      cap_.set(CV_CAP_PROP_BRIGHTNESS, config_.brightness/8.0);
      cap_.set(CV_CAP_PROP_CONTRAST, config_.contrast/8.0);
      cap_.set(CV_CAP_PROP_GAIN, config_.gain/8.0);
      cap_.set(CV_CAP_PROP_HUE, config_.hue/8.0);
      //cap_.set(CV_CAP_PROP_WHITE_BALANCE_RED_V, config_.white_balance);
      cap_.set(CV_CAP_PROP_SATURATION, config_.saturation/8.0);

      cap_.set(CV_CAP_PROP_FPS, config_.frame_rate);

      frame_rate_ = config_.frame_rate;
  }

  void readZedInfo()
  {
      #if USE_ZED_SDK
      sl::zed::Camera *zed;
      zed = new sl::zed::Camera(static_cast<sl::zed::ZEDResolution_mode> (sl::zed::VGA));
      zed->init(sl::zed::MODE::NONE, -1, true);
      // Read ZED serial number, etc.
      serial = str(format("%d") % zed->getZEDSerial());
      fw_version = zed->getZEDFirmware();
      delete zed;
      #endif
  }


  virtual void onInit()
  {
    ros::NodeHandle &nh = getMTNodeHandle();
    ros::NodeHandle &pnh = getMTPrivateNodeHandle();

    ros::NodeHandle lnh(getMTNodeHandle(), "left");
    ros::NodeHandle rnh(getMTNodeHandle(), "right");

    pnh.param<bool>("publish_left", publish_left_, true);
    pnh.param<bool>("publish_right", publish_right_, true);

    pnh.param<int>("device_id", device_id, 0);
    bool scan_devices = device_id==-1;
    int max_device_id;
    pnh.param<int>("max_device_id", max_device_id, 10);

    pnh.param<std::string>("resolution", resolution, "720p");

    std::string l_camera_info_url;
    std::string r_camera_info_url;
    pnh.param<std::string>("left_camera_info_url", l_camera_info_url, "");
    pnh.param<std::string>("right_camera_info_url", r_camera_info_url, "");

    pnh.param<std::string>("frame_id", frame_id_, "camera");

    pnh.param<std::string>("frame_id_left", l_frame_id_, frame_id_);
    pnh.param<std::string>("frame_id_right", r_frame_id_, frame_id_);

    bool read_zed_info;
    pnh.param<bool>("read_zed_info", read_zed_info, false);

    pnh.param<std::string>("serial", serial, serial);

    if(read_zed_info) {
        readZedInfo();
    }

    if(scan_devices) {
        device_id = 0;
    }

    // Try connecting to the camera
    while(!cap_.isOpened() && ros::ok())
    {
      try
      {
        NODELET_INFO("Connecting to camera. Device id: %d", device_id);
        cap_.open(device_id);
        break;
      }
      catch(std::runtime_error& e)
      {
        NODELET_ERROR("%s", e.what());
      }
      if(scan_devices) {
          device_id++;
          if(device_id>max_device_id) {
              device_id = 0;
              ros::Duration(1.0).sleep(); // sleep for one second each time
          }
      } else {
          ros::Duration(1.0).sleep(); // sleep for one second each time
      }
    }

    std::string l_camera_id = str( format("ZED_%s_left_%s") % serial % resolution );
    std::string r_camera_id = str( format("ZED_%s_right_%s") % serial % resolution );

    srv_ = boost::make_shared <dynamic_reconfigure::Server<zed_cv_driver::ZedConfig> > (pnh);
    dynamic_reconfigure::Server<zed_cv_driver::ZedConfig>::CallbackType f =  boost::bind(&zed_cv_driver::ZedCvNodelet::paramCallback, this, _1, _2);
    srv_->setCallback(f);

    l_cinfo_.reset(new camera_info_manager::CameraInfoManager(lnh, l_camera_id, l_camera_info_url));
    l_ci_.reset(new sensor_msgs::CameraInfo(l_cinfo_->getCameraInfo()));
    l_ci_->header.frame_id = l_frame_id_;
    l_it_.reset(new image_transport::ImageTransport(lnh));
    l_it_pub_ = l_it_->advertiseCamera("image_raw", 5);

    r_cinfo_.reset(new camera_info_manager::CameraInfoManager(rnh, r_camera_id, r_camera_info_url));
    r_ci_.reset(new sensor_msgs::CameraInfo(r_cinfo_->getCameraInfo()));
    r_ci_->header.frame_id = r_frame_id_;
    r_it_.reset(new image_transport::ImageTransport(rnh));
    r_it_pub_ = r_it_->advertiseCamera("image_raw", 5);

    topic_diagnostic_ = boost::shared_ptr< diagnostic_updater::TopicDiagnostic > (new diagnostic_updater::TopicDiagnostic(
              "image_raw",
              updater_,
              diagnostic_updater::FrequencyStatusParam(&frame_rate_, &frame_rate_, 0.1, 10),
              diagnostic_updater::TimeStampStatusParam(-0.01, 0.1)));

    // Set up diagnostics
    updater_.setHardwareID(str(format("ZED %s") % serial));
    updater_.add("Device ID", this, &ZedCvNodelet::deviceStatus);


    volatile bool started = false;
    while(!started)
    {
      try
      {
        NODELET_INFO("Starting camera capture.");
        started = true;
        // Start the thread to loop through and publish messages
        pubThread_ = boost::shared_ptr< boost::thread > (new boost::thread(boost::bind(&zed_cv_driver::ZedCvNodelet::devicePoll, this)));
      }
      catch(std::runtime_error& e)
      {
        NODELET_ERROR("%s", e.what());
        ros::Duration(1.0).sleep();
      }
    }
  }

  void deviceStatus(diagnostic_updater::DiagnosticStatusWrapper& status)
  {
    status.summary(0, "ZED is running");
    status.add("Device ID", device_id);
    status.add("Serial", serial);
    status.add("FW Version", fw_version);
    status.add("Resolution", resolution);

  }



  void cleanUp()
  {
    try
    {
      NODELET_DEBUG("Stopping camera capture.");
      NODELET_DEBUG("Disconnecting from camera.");
    }
    catch(std::runtime_error& e)
    {
      NODELET_ERROR("%s", e.what());
    }
  }

  void devicePoll()
  {
      cv::Mat frame;

      while(!boost::this_thread::interruption_requested())
      {
          if(do_reconfigure_) {
              reconfigure();
              do_reconfigure_ = false;
          }
          NODELET_DEBUG("Polling camera");

          cap_.read(frame);

          ros::Time t = ros::Time::now();

          int w = frame.cols;
          int h = frame.rows;

          bridge_.encoding = enc::BGR8;
          bridge_.header.stamp = t;

          if(publish_left_) {
              l_ci_->header.stamp = t;
              l_ci_->height = h;
              l_ci_->width = w/2;

              bridge_.header.frame_id = l_frame_id_;
              bridge_.image = frame(cv::Rect(0, 0, w/2, h));

              l_it_pub_.publish(*bridge_.toImageMsg(), *l_ci_);
              NODELET_DEBUG("Published left");
          }
          if(publish_right_) {
              r_ci_->header.stamp = t;
              r_ci_->height = h;
              r_ci_->width = w/2;

              bridge_.header.frame_id = r_frame_id_;
              bridge_.image = frame(cv::Rect(w/2, 0, w/2, h));

              r_it_pub_.publish(*bridge_.toImageMsg(), *r_ci_);
              NODELET_DEBUG("Published right");
          }

          topic_diagnostic_->tick(t);

          updater_.update();

          ros::Duration(0.005).sleep();
      }
  }

  bool publish_left_;
  bool publish_right_;

  boost::shared_ptr<dynamic_reconfigure::Server<zed_cv_driver::ZedConfig> > srv_;
  cv::VideoCapture cap_;
  cv_bridge::CvImage bridge_;

  std::string resolution;

  std::string frame_id_;
  std::string l_frame_id_;
  std::string r_frame_id_;

  boost::shared_ptr<image_transport::ImageTransport> l_it_;
  boost::shared_ptr<image_transport::ImageTransport> r_it_;

  boost::shared_ptr<camera_info_manager::CameraInfoManager> l_cinfo_;
  boost::shared_ptr<camera_info_manager::CameraInfoManager> r_cinfo_;

  image_transport::CameraPublisher l_it_pub_;
  image_transport::CameraPublisher r_it_pub_;

  sensor_msgs::CameraInfoPtr l_ci_;
  sensor_msgs::CameraInfoPtr r_ci_;

  diagnostic_updater::Updater updater_;
  boost::shared_ptr<diagnostic_updater::TopicDiagnostic> topic_diagnostic_;

  double frame_rate_;


  boost::shared_ptr<boost::thread> pubThread_;

  zed_cv_driver::ZedConfig config_;
  bool do_reconfigure_;

  int device_id;
  std::string serial;
  int fw_version;


};

PLUGINLIB_DECLARE_CLASS(zed_cv_driver, ZedCvNodelet, zed_cv_driver::ZedCvNodelet, nodelet::Nodelet);

}
