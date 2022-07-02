/**
 * @file
 *
 * @author Carlo Rizzardo (crizz, cr.git.mail@gmail.com)
 *
 * Collection of utility methods used by the optar module
 *
 * functions documentation in utils.cpp
 */
#ifndef OPTAR_UTILS_HPP_06032019
#define OPTAR_UTILS_HPP_06032019

#include <tf2_geometry_msgs/tf2_geometry_msgs.h>
#include <ros/ros.h>
#include <cv_bridge/cv_bridge.h>
#include <Eigen/Dense>
#include <opencv2/core/eigen.hpp>
#include <visualization_msgs/Marker.h>
#include <opencv2/core/core.hpp>
#include <tf/transform_listener.h>
#include <chrono>


cv::Point3f get3dPoint(int x, int y, int depth_mm, double focalLengthX, double focalLengthY, double principalPointX, double principalPointY);
void opencvPoseToEigenPose(const cv::Vec3d& rvec, const cv::Vec3d& tvec, Eigen::Vector3d &translation, Eigen::Quaterniond &quaternion);
void tfPoseToOpenCvPose(const tf::Pose& pose, cv::Vec3d& rvec, cv::Vec3d& tvec);
int publish_pose_for_viewing(float tx, float ty, float tz, float qx, float qy, float qz, float qw, ros::Publisher pose_marker_pub, std::string name, float r, float g, float b, float a, float size);
double poseDistance(geometry_msgs::Pose pose1, geometry_msgs::Pose pose2);
double poseDistance(tf::Pose pose1, tf::Pose pose2);

visualization_msgs::Marker buildMarker(const geometry_msgs::Pose& pose, std::string name, float r, float g, float b, float a, float size, std::string frame_id);
visualization_msgs::Marker buildMarker(const cv::Point3f& position, std::string name, float r, float g, float b, float a, float size, std::string frame_id);
visualization_msgs::Marker buildMarker(float x, float y, float z, std::string name, float r, float g, float b, float a, float size, std::string frame_id);
visualization_msgs::Marker buildDeletingMarker(std::string name);
visualization_msgs::Marker buildArrowMarker(float x, float y, float z, std::string name, float r, float g, float b, float a, float size, std::string frame_id,float orient_x,float orient_y,float orient_z,float orient_w);

cv::Point2i findNearestNonZeroPixel(const cv::Mat& image, int x, int y, double maxDist);
cv::Point2i findLowestNonZeroInRing(const cv::Mat& image, int x, int y, double maxRadius, double minRadius);

void transformCvPoint3f(const cv::Point3f& in, cv::Point3f& out, tf::StampedTransform transform);
void prepareOpencvImageForShowing(std::string winName, cv::Mat image, int winHeight, int winWidth=-1);
void publishPoseAsTfFrame(const geometry_msgs::PoseStamped& pose, std::string tfFrameName);
void publishTransformAsTfFrame(const tf::Transform& transform, std::string tfFrameName, std::string parentFrame, const ros::Time& time);
void publishTransformAsTfFrame(const tf::StampedTransform& stampedTransform);


geometry_msgs::Point buildRosPoint(double positionX, double positionY, double positionZ);
geometry_msgs::Quaternion buildRosQuaternion(double quaternionX, double quaternionY, double quaternionZ, double quaternionW);
geometry_msgs::Pose buildRosPose(double positionX, double positionY, double positionZ, double quaternionX, double quaternionY, double quaternionZ, double quaternionW);
geometry_msgs::Pose buildRosPose(const Eigen::Vector3d& position, const Eigen::Quaterniond& orientation);

std::string poseToString(tf::Pose pose);
std::string poseToString(geometry_msgs::Pose pose);


tf::Transform convertPoseUnityToRos(const tf::Transform& leftHandedPose);
tf::Vector3 averagePosePositions(std::vector<tf::Pose> poses);

bool isPoseValid(const tf::Pose& pose);

geometry_msgs::PoseStamped poseToPoseStamped(const geometry_msgs::Pose& pose, std::string frame_id, ros::Time timestamp);

tf::Pose convertCameraPoseArcoreToRos(const geometry_msgs::Pose& cameraPoseArcore);
tf::Pose convertCameraPoseArcoreToRos(const tf::Pose& cameraPoseArcore);

geometry_msgs::Pose invertPose(const geometry_msgs::Pose& pose);

// added by peter
namespace profiling // NOTE: not thread-safe!
{

using namespace std::chrono;
using profile_clock=std::chrono::high_resolution_clock;

static profile_clock::time_point t, snapT;

struct helper {
    helper():start_(profile_clock::now()) { lastSnap_ = start_; }
    ~helper() { }

    template<class CastT = milliseconds>
    double snap() {
        auto now = profile_clock::now();
        duration<double, typename CastT::period> d = now - lastSnap_;
        lastSnap_ = now;
        return d.count();
    }

    template<class CastT = milliseconds>
    double total() {
        duration<double, typename CastT::period> d = profile_clock::now() - start_;
        return d.count();
    }

    profile_clock::time_point start_, lastSnap_;
};

// inline void start() { t = high_resolution_clock::now(); snapT = t; }

// template<class CastT>
// inline int snap() {
//     auto now = high_resolution_clock::now();
//     auto d = duration_cast<CastT>(now - snapT).count();
//     snapT = now;
//     return (int)d;
// }

// template<class CastT>
// inline int total() { return (int)duration_cast<CastT>(high_resolution_clock::now() - t).count(); }
}

#define PH_START() struct profiling::helper ph ## __func__
#define PH_SNAP()(ph##__func__.snap())
#define PH_TOTAL()(ph##__func__.total())

#endif
