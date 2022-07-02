/**
 * @file
 *
 *
 * @author Carlo Rizzardo (crizz, cr.git.mail@gmail.com)
 *
 *
 * CameraPoseEstimator methods implementation file
 */

#include "CameraPoseEstimator.hpp"
#include <opencv2/highgui/highgui.hpp>
#include <tf2_geometry_msgs/tf2_geometry_msgs.h>
#include <ros/ros.h>
#include <opt_msgs/ArcoreCameraImage.h>
#include <sensor_msgs/Image.h>
#include <sensor_msgs/CameraInfo.h>
#include <opencv2/xphoto.hpp>
#include <cv_bridge/cv_bridge.h>
#include <opencv2/features2d/features2d.hpp>
#include <chrono>
#include <tf/transform_listener.h>
#include <Eigen/Dense>
#include <opencv2/calib3d.hpp>
#include <opencv2/core/eigen.hpp>
#include <thread> // std::this_thread::sleep_for
#include <visualization_msgs/Marker.h>
#include <visualization_msgs/MarkerArray.h>
#include <tf/transform_broadcaster.h>
#include <image_transport/image_transport.h>

#include "../../utils.hpp"

using namespace std;
using namespace cv;

/**
 * Constructs the estimator
 * @param ARDeviceId                  Id of the AR device for which we are estimating the registration
 * @param nh                          ROS node handle
 * @param transformFixedCameraToWorld Tf transform between the fixed camera and the /world frame
 * @param fixed_sensor_name           Sensor name of the fixed camera
 * @param featuresMemory              Features memory to use
 */
CameraPoseEstimator::CameraPoseEstimator(string ARDeviceId,
										 ros::NodeHandle &nh,
										 geometry_msgs::TransformStamped transformFixedCameraToWorld,
										 std::string fixed_sensor_name,
										 std::shared_ptr<FeaturesMemory> featuresMemory)
{
	this->transformFixedCameraToWorld = transformFixedCameraToWorld;
	this->ARDeviceId = ARDeviceId;
	this->fixed_sensor_name = fixed_sensor_name;
	this->featuresMemory = featuresMemory;

	// pose_raw_pub = nh.advertise<geometry_msgs::PoseStamped>("optar/"+ARDeviceId+"/"+outputPoseRaw_topicName, 10);
	debug_markers_pub = nh.advertise<visualization_msgs::MarkerArray>("optar/" + ARDeviceId + "/" + outputPoseMarker_topicName, 1);

	image_transport::ImageTransport it(nh);
	matches_images_pub = it.advertise("optar/" + ARDeviceId + "/img_matches_" + fixed_sensor_name, 1);
	reproj_images_pub = it.advertise("optar/" + ARDeviceId + "/img_reprojection_" + fixed_sensor_name, 1);
}

/**
 * Updates the parameters used to perform the estimation
 * @param pnpReprojectionError                    Reprojection error threshold used for the solvePnPRansac() method (see OpenCV docs)
 * @param pnpConfidence                           Confidence value used in the solvePnPRansac() method (see OpenCV docs)
 * @param pnpIterations                           Iterations amount used in the solvePnPRansac method (see OpenCV docs)
 * @param matchingThreshold                       Threshold for considering to ORB features to be matching (it's the maximum distance
 *                                                between descriptors, so lower implies less accepted matches)
 * @param reprojectionErrorDiscardThreshold       Threshold to discard estimates based on their reprojection error (it's the maximum error,
 *                                                so lower implies less accepted estimates)
 * @param orbMaxPoints                            Maximum number of ORB features that are extracted. If using phone-side features extraction
 *                                                then this influences only the fixed camera features. Also, the features coming from memory
 *                                                are not limited by this)
 * @param orbScaleFactor                          Scale factor used in the ORB features computation, must be more than 1. Lower implies
 *                                                more features are extracted at more scales, which is good, but it slows down the
 *                                                computation. If using phone-side features extraction then this influences only the fixed
 *                                                camera features.
 * @param orbLevelsNumber                         Number of scale levels on which the features are extracted. If you make
 *                                                ARDeviceRegistration#orbScaleFactor lower then you should make this higher.
 * @param phoneOrientationDifferenceThreshold_deg If the angle between the estimated mobile camera optical axis and the fixed camera optical
 *                                                axis is beyond this threshold then the estimation is rejected. It's in degrees.
 * @param showImages                              Controls if the debug images are published
 * @param minimumMatchesNumber                    Minimum number of matches between mobile side and fixed side needed to accept an estimate.
 *                                                To compute the mobile camera position at least 4 mathces are needed, so if you set this below
 *                                                4 it will be as if it was 4.
 * @param enableFeaturesMemory                    Controls if the feature memory is used
 * @param maxPoseHeight                           Maximum allowed height for a pose estimate, estimates exceeding this thresghold are discarded
 * @param minPoseHeight                           Minimum allowed height for a pose estimate, estimates exceeding this thresghold are discarded
 */
void CameraPoseEstimator::setupParameters(double pnpReprojectionError,
										  double pnpConfidence,
										  double pnpIterations,
										  double matchingThreshold,
										  double reprojectionErrorDiscardThreshold,
										  int orbMaxPoints,
										  double orbScaleFactor,
										  int orbLevelsNumber,
										  double phoneOrientationDifferenceThreshold_deg,
										  bool showImages,
										  unsigned int minimumMatchesNumber,
										  bool enableFeaturesMemory,
										  double maxPoseHeight,
										  double minPoseHeight)
{
	this->pnpReprojectionError = pnpReprojectionError;
	this->pnpConfidence = pnpConfidence;
	this->pnpIterations = pnpIterations;
	this->matchingThreshold = matchingThreshold;
	this->reprojectionErrorDiscardThreshold = reprojectionErrorDiscardThreshold;
	this->orbMaxPoints = orbMaxPoints;
	this->orbScaleFactor = orbScaleFactor;
	this->orbLevelsNumber = orbLevelsNumber;
	this->phoneOrientationDifferenceThreshold_deg = phoneOrientationDifferenceThreshold_deg;
	this->showImages = showImages;
	this->minimumMatchesNumber = minimumMatchesNumber;
	this->enableFeaturesMemory = enableFeaturesMemory;
	this->maxPoseHeight = maxPoseHeight;
	this->minPoseHeight = minPoseHeight;
}

/**
 * Updates the estimate using precomputed fetures from the mobile camera and live images from
 * the fixed camera. And, if enabled, the features memory
 *
 * @param  arcoreInputMsg       Message from the mobile camera, containing precomputed features,
 *                              camera info, and camera position in the mobile frame
 * @param  kinectInputCameraMsg Regular mono image from the fixed camera
 * @param  kinectInputDepthMsg  Depth image from the fixed camera
 * @param  kinectCameraInfo     Camera info for the fixed camera
 * @return                      0 on success, a negative value on fail
 */
int CameraPoseEstimator::featuresCallback(const opt_msgs::ArcoreCameraFeaturesConstPtr &arcoreInputMsg,
										  const sensor_msgs::ImageConstPtr &kinectInputCameraMsg,
										  const sensor_msgs::ImageConstPtr &kinectInputDepthMsg,
										  const sensor_msgs::CameraInfo &kinectCameraInfo)
{
	PH_START();

	long arcoreTime = arcoreInputMsg->header.stamp.sec * 1000000000L + arcoreInputMsg->header.stamp.nsec;
	long kinectTime = kinectInputCameraMsg->header.stamp.sec * 1000000000L + kinectInputCameraMsg->header.stamp.nsec;
	ROS_INFO_STREAM("Parameters: " << endl
								   << "pnp iterations = " << pnpIterations << endl
								   << "pnp confidence = " << pnpConfidence << endl
								   << "pnp reporjection error = " << pnpReprojectionError << endl
								   << "matching threshold =" << matchingThreshold << endl
								   << "reprojection discard threshold =" << reprojectionErrorDiscardThreshold << endl
								   << "orb max points = " << orbMaxPoints << endl
								   << "orb scale factor = " << orbScaleFactor << endl
								   << "orb levels number = " << orbLevelsNumber << endl
								   << "phone orientation difference threshold = " << phoneOrientationDifferenceThreshold_deg << endl
								   << "enableFeaturesMemory = " << enableFeaturesMemory << endl
								   << "show images = " << showImages);

	ROS_DEBUG("Received images. time diff = %+7.5f sec.  arcore time = %012ld  kinect time = %012ld", (arcoreTime - kinectTime) / 1000000000.0, arcoreTime, kinectTime);

	//:::::::::::::::Decode received images and stuff::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
	int r;
	cv::Mat arcoreCameraMatrix;
	cv::Mat arcoreDescriptors;
	std::vector<cv::KeyPoint> arcoreKeypoints;
	cv::Size arcoreImageSize;
	cv::Mat kinectCameraMatrix;
	cv::Mat kinectCameraImg;
	cv::Mat kinectDepthImg;
	cv::Mat arcoreImage;
	r = readReceivedMessages_features(arcoreInputMsg, kinectInputCameraMsg, kinectInputDepthMsg, kinectCameraInfo,
									  arcoreCameraMatrix,
									  arcoreDescriptors,
									  arcoreKeypoints,
									  arcoreImageSize,
									  kinectCameraMatrix,
									  kinectCameraImg,
									  kinectDepthImg,
									  arcoreImage);

	if (r < 0)
	{
		ROS_ERROR("Invalid input messages. Dropping frame");
		return -1;
	}

	//:::::::::::::::Compute the features in the images::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
	PH_SNAP();
	// find matches
	std::vector<cv::KeyPoint> fixedKeypoints;
	cv::Mat kinectDescriptors;
	r = computeOrbFeatures(kinectCameraImg, fixedKeypoints, kinectDescriptors);
	if (r < 0)
	{
		ROS_ERROR("error computing camera features");
		return -2;
	}
	
	ROS_INFO("ros side ORB computation %.3fms", PH_SNAP());
	
	if (enableFeaturesMemory)
	{
		const vector<FeaturesMemory::Feature> featuresFromMemory = featuresMemory->getFeatures();
		ROS_INFO_STREAM("got " << featuresFromMemory.size() << " features from memory");
		for (const FeaturesMemory::Feature &feature : featuresFromMemory)
		{
			fixedKeypoints.push_back(feature.keypoint);
			kinectDescriptors.push_back(feature.descriptor);
		}
	}

	r = 10 * update(arcoreKeypoints,
					arcoreDescriptors,
					fixedKeypoints,
					kinectDescriptors,
					arcoreImageSize,
					kinectCameraImg.size(),
					arcoreCameraMatrix,
					kinectCameraMatrix,
					kinectDepthImg,
					kinectCameraImg,
					arcoreImage,
					arcoreInputMsg->header.stamp,
					kinectInputCameraMsg->header.frame_id);

	ROS_INFO("total duration is %.3f ms", PH_TOTAL());
	return r;
}

/**
 * Estimates the transformation using the descriptors and keypoints from the fixed camera
 * and the AR device. If CameraPoseEstimator#showImages is set it will also publish
 * an image showing the matches between the images.
 *
 * @param[in]  arcoreKeypoints                The keypoints in the AR device image
 * @param[in]  arcoreDescriptors              The descriptors corrwsponding to the AR device keypoints
 * @param[in]  fixedKeypoints                 The keypoints in the fixed camera image
 * @param[in]  fixedDescriptors               The descriptors corresponding to the fixed camera keypoints
 * @param[in]  arcoreImageSize                The pixel size of the AR device image
 * @param[in]  kinectImageSize                The pixel size of the fixed camera image
 * @param[in]  arcoreCameraMatrix             The camera matrix for the AR device camera
 * @param[in]  fixedCameraMatrix              The camera matrix for the fixed camera
 * @param      kinectDepthImage               The fixed camera depth image
 * @param[in]  kinectMonoImage                The fixed camera monochrome image
 * @param[in]  arcoreImageDbg                 The image from the AR device camera, only here for debug
 *                                            purpouses.You can pass a null Mat if you want. The idea
 *                                            is to pass here a super-low resolution image to display
 *                                            in the matches_img
 * @param[in]  timestamp                      The timestamp of the images. The images should be all
 *                                            roughly from the same instant
 * @param[in]  fixedCameraFrameId             The fixed camera tf frame_id
 *
 * @return     returns zero in case of success, a negative value in case of an internal error, a
 *             positive value greater than zero if it couldn't determine the transformation because
 *             the device is looking at something too different to what the fixed camera is seeing.
 */
int CameraPoseEstimator::update(const std::vector<cv::KeyPoint> &arcoreKeypoints,
								const cv::Mat &arcoreDescriptors,
								const std::vector<cv::KeyPoint> &fixedKeypoints,
								const cv::Mat &fixedDescriptors,
								const cv::Size &arcoreImageSize,
								const cv::Size &kinectImageSize,
								const cv::Mat &arcoreCameraMatrix,
								const cv::Mat &fixedCameraMatrix,
								cv::Mat &kinectDepthImage,
								const cv::Mat &kinectMonoImage,
								const cv::Mat &arcoreImageDbg,
								const ros::Time &timestamp,
								const std::string fixedCameraFrameId)
{
	PH_START();

	// if arcoreImage is not set just use a black image, it's just for visualization
	cv::Mat arcoreImage;
	if (!arcoreImageDbg.data)
		arcoreImage = cv::Mat(arcoreImageSize.height, arcoreImageSize.width, CV_8UC1, Scalar(0, 0, 0));
	else
		arcoreImage = arcoreImageDbg;

	if (enableFeaturesMemory)
	{
		featuresMemory->removeNonBackgroundFeatures(kinectDepthImage);
		ROS_DEBUG("-- feature memory non-bg removal %.3fms", PH_SNAP());
	}

	std::chrono::steady_clock::time_point beforeMatching = std::chrono::steady_clock::now();

	// find orb matches between arcore and fixed-camera features
	std::vector<cv::DMatch> matches;
	int r = findOrbMatches(arcoreDescriptors, fixedDescriptors, matches);
	if (r < 0)
	{
		ROS_ERROR("error finding matches");
		return -1;
	}
	
	ROS_DEBUG("-- find ORB matches %.3fms", PH_SNAP());
	ROS_DEBUG("got %d matches", matches.size());

	// filter matches
	std::vector<cv::DMatch> goodMatchesWithNull;
	r = filterMatches(matches, goodMatchesWithNull, arcoreKeypoints, fixedKeypoints);
	if (r < 0)
	{
		ROS_ERROR("error filtering matches");
		return -2;
	}
	ROS_DEBUG("-- filter ORB matches %.3fms", PH_SNAP());
	ROS_DEBUG("Got %lu good matches, but some could be invalid", goodMatchesWithNull.size());

	// On the kinect side the depth could be zero at the match location
	// we try to get the nearest non-zero depth, if it's too far we discard the match
	std::vector<cv::DMatch> goodMatches;
	r = fixMatchesDepthOrDrop(goodMatchesWithNull, fixedKeypoints, kinectDepthImage, goodMatches);
	if (r < 0)
	{
		ROS_ERROR("error fixing matches depth");
		return -3;
	}
	ROS_DEBUG("-- fixing matches depth %.3fms", PH_SNAP());
	ROS_INFO_STREAM("got " << goodMatches.size() << " actually good matches");

	//:::::::::::::::Find the 3d position of the matches::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
	// find the 3d poses corresponding to the goodMatches, these will be relative to the kinect frame
	std::vector<cv::Point3f> goodMatches3dPos;
	std::vector<cv::Point2f> goodMatchesImgPos;
	r = get3dPositionsAndImagePositions(goodMatches, fixedKeypoints, arcoreKeypoints, kinectDepthImage, 
		fixedCameraMatrix, goodMatches3dPos, goodMatchesImgPos);
	if (r < 0)
	{
		ROS_ERROR("error getting matching points");
		return -4;
	}
	ROS_DEBUG("-- matches 3D reconstruction %.3fms", PH_SNAP());

	// send markers to rviz and publish matches image
	sensor_msgs::ImagePtr matchesDebugImg;
	
	if (showImages)
	{
		// /optar/4E420E77_949A_44B5_A43A_162E97928BC5/img_matches_zed_head
		cv::Mat matchesImg;
		cv::drawMatches(arcoreImage, arcoreKeypoints, kinectMonoImage, fixedKeypoints, goodMatches, matchesImg,
						cv::Scalar::all(-1), cv::Scalar::all(-1), std::vector<char>(),
						cv::DrawMatchesFlags::DEFAULT /*cv::DrawMatchesFlags::DRAW_RICH_KEYPOINTS*/);
		cv::putText(matchesImg, std::to_string(goodMatches.size()).c_str(), cv::Point(0, matchesImg.rows - 5), FONT_HERSHEY_SIMPLEX, 2, Scalar(255, 0, 0), 3);
		matches_images_pub.publish(cv_bridge::CvImage(std_msgs::Header(), "bgr8", matchesImg).toImageMsg());

		ROS_DEBUG("-- publish matches img topic %.3fms", PH_SNAP());
	}

	// If we have less than 4 matches we cannot procede, pnp wouldn't be able to estimate the phone position
	if (goodMatches.size() < 4 || goodMatches.size() < minimumMatchesNumber)
	{
		ROS_WARN("not enough good matches to determine position");
		return 1;
	}

	//:::::::::::::::Determine the phone position::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
	//----------------------------------------------------------------------
	/*
		std::vector<int> inliers;
		std::chrono::steady_clock::time_point beforePnPComputation = std::chrono::steady_clock::now();
		geometry_msgs::Pose cameraPose_fixedCameraFrame;
		r = computeMobileCameraPose(arcoreCameraMatrix,goodMatches3dPos,goodMatchesImgPos, inliers, cameraPose_fixedCameraFrame);
		std::chrono::steady_clock::time_point afterPnPComputation = std::chrono::steady_clock::now();

		std::chrono::steady_clock::time_point beforeReprojection = std::chrono::steady_clock::now();
		std::vector<cv::Point2f> reprojectedPoints;
		double reprojectionError = computeReprojectionError(cameraPose_fixedCameraFrame,goodMatches3dPos, arcoreCameraMatrix, goodMatchesImgPos, inliers, reprojectedPoints);
		std::chrono::steady_clock::time_point afterReprojection = std::chrono::steady_clock::now();
		ROS_DEBUG("Reprojection error computation took %lu ms",std::chrono::duration_cast<std::chrono::milliseconds>(afterReprojection - beforeReprojection).count());

	*/

	// ROS_DEBUG_STREAM("arcoreCameraMatrix = \n"
	// 				 << arcoreCameraMatrix);
	cv::Vec3d tvec;
	cv::Vec3d rvec;
	bool usePreviousEstimate = false;
	if (didComputeEstimate) // initialize with the previous estimate
	{
		tf::Pose lastEstimateTf;
		tf::poseMsgToTF(lastPoseEstimate.pose, lastEstimateTf);
		tfPoseToOpenCvPose(lastEstimateTf, rvec, tvec);
		usePreviousEstimate = true;
	}
	std::vector<int> inliers;
	ROS_DEBUG_STREAM("Running pnpRansac with iterations=" << pnpIterations << " pnpReprojectionError=" << pnpReprojectionError << " pnpConfidence=" << pnpConfidence);

	bool rb = cv::solvePnPRansac(goodMatches3dPos, goodMatchesImgPos,
								 arcoreCameraMatrix, cv::noArray(),
								 rvec, tvec,
								 usePreviousEstimate,
								 pnpIterations,
								 pnpReprojectionError,
								 pnpConfidence,
								 inliers);
	if (!rb)
		r = -1;
	ROS_DEBUG_STREAM("solvePnPRansac used " << inliers.size() << " inliers and says:\t tvec = " << tvec.t() << "\t rvec = " << rvec.t());
	if (inliers.size() < 4 || inliers.size() < minimumMatchesNumber)
	{
		ROS_WARN_STREAM("Not enough match inliers (" << inliers.size() << "<" << minimumMatchesNumber << "). Skipping frame");
		return 2;
	}

	ROS_DEBUG("-- PnP Ransac computation %.3fms", PH_SNAP());

	// reproject points to then check reprojection error (and visualize them)
	std::vector<Point2f> reprojectedPoints;
	cv::projectPoints(goodMatches3dPos,
					  rvec, tvec,
					  arcoreCameraMatrix,
					  cv::noArray(),
					  reprojectedPoints);
	// debug image to show the reprojection errors

	double reprojectionError = 0;
	// calculate reprojection error mean and draw reprojections
	for (unsigned int i = 0; i < inliers.size(); i++)
	{
		Point2f pix = goodMatchesImgPos.at(inliers.at(i));
		Point2f reprojPix = reprojectedPoints.at(inliers.at(i));
		reprojectionError += hypot(pix.x - reprojPix.x, pix.y - reprojPix.y) / inliers.size();
	}

	// convert to ros format
	Eigen::Vector3d position;
	Eigen::Quaterniond rotation;
	opencvPoseToEigenPose(rvec, tvec, position, rotation);
	geometry_msgs::Pose cameraPose_fixedCameraFrame = invertPose(buildRosPose(position, rotation));

	ROS_INFO_STREAM("inliers (#=" << inliers.size() << ") reprojection error = " << reprojectionError);

	visualization_msgs::MarkerArray markerArray;
	for (unsigned int i = 0; i < inliers.size(); i++)																									  // build the rviz markers
		markerArray.markers.push_back(buildMarker(goodMatches3dPos.at(inliers.at(i)), "match" + std::to_string(i), 0, 0, 1, 1, 0.2, fixedCameraFrameId)); // matches are blue
	debug_markers_pub.publish(markerArray);

	ROS_DEBUG("-- debug markers publish %.3fms", PH_SNAP());

	//----------------------------------------------------------------
	if (showImages) // /optar/4E420E77_949A_44B5_A43A_162E97928BC5/img_reprojection_zed_head
	{
		drawAndSendReproectionImage(arcoreImage, inliers, goodMatchesImgPos, reprojectedPoints);
		ROS_DEBUG("-- draw&publish debug reprojection img %.3fms", PH_SNAP());
	}
	
	if (r < 0)
	{
		ROS_ERROR("Failed to compute pose");
		return -5;
	}

	// discard bad frames
	if (reprojectionError > reprojectionErrorDiscardThreshold)
	{
		ROS_WARN("Reprojection error %.3f beyond threshold %.3f, aborting estimation",
			reprojectionError, reprojectionErrorDiscardThreshold);
		return 3;
	}

	// transform to world frame
	geometry_msgs::PoseStamped phonePose_world;
	tf2::doTransform(poseToPoseStamped(cameraPose_fixedCameraFrame, fixed_sensor_name + "_rgb_optical_frame", timestamp), phonePose_world, transformFixedCameraToWorld);
	phonePose_world.header.frame_id = "/world";
	phonePose_world.header.stamp = timestamp;
	// pose_raw_pub.publish(phonePose_world);
	ROS_DEBUG_STREAM("estimated pose is                " << phonePose_world.pose.position.x << " " << phonePose_world.pose.position.y << " " << phonePose_world.pose.position.z << " ; " << phonePose_world.pose.orientation.x << " " << phonePose_world.pose.orientation.y << " " << phonePose_world.pose.orientation.z << " " << phonePose_world.pose.orientation.w);

	if (phonePose_world.pose.position.z > maxPoseHeight)
	{
		ROS_WARN_STREAM("Pose height above max threshold. discarding");
		return 4;
	}
	if (phonePose_world.pose.position.z < minPoseHeight)
	{
		ROS_WARN_STREAM("Pose height below min threshold. discarding");
		return 5;
	}
	// TODO: this check should not be done if we use the features memory, which should
	//  ideally be always.
	// compute orientation difference with respect to the fixed camera
	double phoneToCameraRotationAngle = computeAngleFromZAxis(cameraPose_fixedCameraFrame);
	ROS_DEBUG_STREAM("Angle = " << phoneToCameraRotationAngle);
	if (phoneToCameraRotationAngle > phoneOrientationDifferenceThreshold_deg)
	{
		ROS_WARN_STREAM("Orientation difference between phone and camera is too high, discarding estimation (" << phoneToCameraRotationAngle << ")");
		return 6;
	}

	// save the features we used to memory, they are useful!
	//	saveInliersToMemory(inliers, goodMatches3dPos, cameraPose_fixedCameraFrame, goodMatches, fixedKeypoints, fixedDescriptors, kinectDepthImage);
	if (enableFeaturesMemory)
		saveInliersToMemory(inliers, goodMatches3dPos, cameraPose_fixedCameraFrame, goodMatches, arcoreKeypoints, arcoreDescriptors, kinectDepthImage);

	lastPoseEstimate = phonePose_world;
	lastEstimateMatchesNumber = goodMatches.size();
	lastEstimateReprojectionError = reprojectionError;
	didComputeEstimate = true;
	//:::::::::::::::Get arcore world frame of reference::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

	// ROS_DEBUG_STREAM("featureMemoryNonBackgroundRemovalDuration=" << std::chrono::duration_cast<std::chrono::milliseconds>(beforeMatching - beforeFeatureMemoryNonBackgroundRemoval).count() << "ms");
	// ROS_DEBUG_STREAM("matchesComputationDuration=" << std::chrono::duration_cast<std::chrono::milliseconds>(afterMatchesComputation - beforeMatching).count() << "ms");
	// ROS_DEBUG_STREAM("_3dPositionsComputationDuration=" << std::chrono::duration_cast<std::chrono::milliseconds>(after3dpositionsComputation - afterMatchesComputation).count() << "ms");
	// //	ROS_DEBUG_STREAM("reprojectionComputationDuration="<<std::chrono::duration_cast<std::chrono::milliseconds>(afterReprojection - beforeReprojection).count()<<"ms");
	// ROS_DEBUG_STREAM("drawMatchesDuration=" << std::chrono::duration_cast<std::chrono::milliseconds>(afterMatchesImage - beforeMatchesimage).count() << "ms");
	// ROS_DEBUG_STREAM("drawReprojection=" << std::chrono::duration_cast<std::chrono::milliseconds>(afterDrawingReproj - beforeDrawingReproj).count() << "ms");
	// ROS_INFO_STREAM("pnpComputationDuration=" << std::chrono::duration_cast<std::chrono::milliseconds>(afterPnPComputation - beforePnPComputation).count() << "ms");

	ROS_DEBUG("-- update total %.3fms", PH_TOTAL());

	return 0;
}

/**
 * Computes the pose of the mobile camera using PnP
 * @return [description]
 */
int CameraPoseEstimator::computeMobileCameraPose(const cv::Mat &mobileCameraMatrix,
												 const std::vector<cv::Point3f> &matches3dPositions,
												 const std::vector<cv::Point2f> &matchesImgPixelPos,
												 std::vector<int> &inliers,
												 geometry_msgs::Pose &resultPose)
{

	ROS_DEBUG_STREAM("arcoreCameraMatrix = \n"
					 << mobileCameraMatrix);
	cv::Vec3d tvec;
	cv::Vec3d rvec;
	if (didComputeEstimate) // initialize with the previous estimate
	{
		tf::Pose lastEstimateTf;
		poseMsgToTF(lastPoseEstimate.pose, lastEstimateTf);
		tfPoseToOpenCvPose(lastEstimateTf, rvec, tvec);
	}

	ROS_DEBUG_STREAM("Running pnpRansac with iterations=" << pnpIterations << " pnpReprojectionError=" << pnpReprojectionError << " pnpConfidence=" << pnpConfidence);
	bool r = cv::solvePnPRansac(matches3dPositions, matchesImgPixelPos,
								mobileCameraMatrix, cv::noArray(),
								rvec, tvec,
								didComputeEstimate,
								pnpIterations,
								pnpReprojectionError,
								pnpConfidence,
								inliers);
	if (!r)
	{
		return -1;
	}
	ROS_DEBUG_STREAM("solvePnPRansac used " << inliers.size() << " inliers and says:\t tvec = " << tvec.t() << "\t rvec = " << rvec.t());

	// ROS_DEBUG("PNP computation took %lu ms",pnpComputationDuration);

	// convert to ros format
	Eigen::Vector3d position;
	Eigen::Quaterniond rotation;
	opencvPoseToEigenPose(rvec, tvec, position, rotation);
	geometry_msgs::Pose poseNotStamped = buildRosPose(position, rotation);

	// invert the pose, because that's what you do
	resultPose = invertPose(poseNotStamped);
	return 0;
}

/**
 * Saves to memory the features indicated in the inliers vector, taking the information from the other arguments
 * @param inliers                     elements to save to memory
 * @param goodMatches3dPos            3d positions of the points
 * @param cameraPose_fixedCameraFrame pose of the mobile camera in the fixed camera frame
 * @param goodMatches                 list containing the opencv matches
 * @param keypoints                   keypoints associated with the provided matches
 * @param descriptors                 descriptors associated with the provided matches
 * @param kinectDepthImage            depth image form the fixed camera
 */
void CameraPoseEstimator::saveInliersToMemory(const std::vector<int> &inliers,
											  const std::vector<cv::Point3f> &goodMatches3dPos,
											  const geometry_msgs::Pose &cameraPose_fixedCameraFrame,
											  const std::vector<DMatch> &goodMatches,
											  const std::vector<cv::KeyPoint> &fixedKeypoints,
											  const cv::Mat &fixedDescriptors,
											  const cv::Mat &kinectDepthImage)
{
	// save the features we used to memory, they are useful!
	for (unsigned int i = 0; i < inliers.size(); i++)
	{
		ROS_INFO_STREAM("Saving inlier #" << i);
		Point3f feature3dPosCv = goodMatches3dPos.at(inliers.at(i));
		tf::Vector3 feature3dPosTf(feature3dPosCv.x, feature3dPosCv.y, feature3dPosCv.z); // this position is in the fixed camera frame
		tf::Pose cameraPoseTf_fixedCameraFrame;
		tf::poseMsgToTF(cameraPose_fixedCameraFrame, cameraPoseTf_fixedCameraFrame);
		tf::Vector3 phonePosition = cameraPoseTf_fixedCameraFrame.getOrigin();
		Point3f phonePositionCv(phonePosition.x(), phonePosition.y(), phonePosition.z());
		const DMatch &match = goodMatches.at(inliers.at(i));

		const KeyPoint &keypoint = fixedKeypoints.at(match.trainIdx);
		const Mat &descriptor = fixedDescriptors.row(match.trainIdx);
		double observerDistance_meters = feature3dPosTf.distance(phonePosition);
		Point3f observerDirection(feature3dPosCv.x - phonePositionCv.x, feature3dPosCv.y - phonePositionCv.y, feature3dPosCv.z - phonePositionCv.z); // this will be in the fixed camera frame
		uint16_t depth = kinectDepthImage.at<uint16_t>(keypoint.pt);

		FeaturesMemory::Feature feature(keypoint, descriptor, observerDistance_meters, observerDirection, depth);
		featuresMemory->saveFeature(feature);
	}
}

/**
 * Computes the angle between the absolute z axis and the z axis local to the proivided pose
 * @param  pose the pose
 * @return      the angle
 */
double CameraPoseEstimator::computeAngleFromZAxis(const geometry_msgs::Pose &pose)
{
	tf::Pose poseTf;
	tf::poseMsgToTF(pose, poseTf);
	tf::Vector3 zUnitVector(0, 0, 1);
	tf::Vector3 opticalAxis = tf::Transform(poseTf.getRotation()) * zUnitVector;
	return std::abs(opticalAxis.angle(zUnitVector)) * 180 / 3.14159;
}

/**
 * Draws and send a representation of the reprojection of the privided points
 * @param arcoreImage        the image on which to draw
 * @param inliers            the points to use
 * @param matchesImgPixelPos the original 2d pixel position
 * @param reprojectedPoints  the reprojected 2d pixel positions
 */
void CameraPoseEstimator::drawAndSendReproectionImage(const cv::Mat &arcoreImage,
													  const std::vector<int> &inliers,
													  const std::vector<cv::Point2f> &matchesImgPixelPos,
													  const std::vector<cv::Point2f> &reprojectedPoints)
{
	std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();

	// if we have to send debug images
	cv::Mat reprojectionImg;
	// draw the reprojection image
	cvtColor(arcoreImage, reprojectionImg, CV_GRAY2RGB);
	for (size_t i = 0; i < inliers.size(); i++)
	{
		// ROS_INFO_STREAM("Getting pix for inlier #"<<i<<" = match #"<<inliers.at(i));
		Point2f pix = matchesImgPixelPos.at(inliers.at(i));
		// ROS_INFO_STREAM("Getting reprojPix for inlier #"<<i<<" = match #"<<inliers.at(i));
		Point2f reprojPix = reprojectedPoints.at(inliers.at(i));
		// ROS_INFO("Got reprojPix");

		int r = ((double)rand()) / RAND_MAX * 255;
		int g = ((double)rand()) / RAND_MAX * 255;
		int b = ((double)rand()) / RAND_MAX * 255;
		Scalar color = Scalar(r, g, b);
		cv::circle(reprojectionImg, pix, 15, color, 5);
		cv::line(reprojectionImg, pix, reprojPix, color, 3);
	}
	cv::putText(reprojectionImg, (std::to_string(inliers.size()) + "/" + std::to_string(matchesImgPixelPos.size())).c_str(), cv::Point(0, reprojectionImg.rows - 5), FONT_HERSHEY_SIMPLEX, 2, Scalar(255, 0, 0), 3);
	sensor_msgs::ImagePtr msgReproj = cv_bridge::CvImage(std_msgs::Header(), "bgr8", reprojectionImg).toImageMsg();

	// send the image
	reproj_images_pub.publish(msgReproj);

	std::chrono::steady_clock::time_point afterDrawMatches = std::chrono::steady_clock::now();
	unsigned long drawMatchesDuration = std::chrono::duration_cast<std::chrono::milliseconds>(afterDrawMatches - start).count();
	ROS_DEBUG("drawing and sending debug images took %lu ms", drawMatchesDuration);
}

/**
 * Computes the reprojection error of the provided 3d points on a camera at the provided pose
 * with the provided matrix
 * @param  pose               the camera pose
 * @param  points3d           the 3d positions of the points
 * @param  mobileCameraMatrix the camera matrix
 * @param  points2d           the correct 2d poses of the points
 * @param  inliers            Indicates which points to use of the provided ones
 * @param  reprojectedPoints  the function returns here the reprojected points
 * @return                    the reprojection error
 */
double CameraPoseEstimator::computeReprojectionError(const geometry_msgs::Pose &pose,
													 const std::vector<cv::Point3f> &points3d,
													 const cv::Mat &mobileCameraMatrix,
													 const std::vector<cv::Point2f> &points2d,
													 const std::vector<int> &inliers,
													 std::vector<cv::Point2f> &reprojectedPoints)
{

	cv::Vec3d tvec;
	cv::Vec3d rvec;
	// invert because yes
	geometry_msgs::Pose invertedPose = invertPose(pose);
	tf::Pose poseTf;
	tf::poseMsgToTF(invertedPose, poseTf);
	tfPoseToOpenCvPose(poseTf, rvec, tvec);

	// reproject points to then check reprojection error (and visualize them)
	cv::projectPoints(points3d,
					  rvec, tvec,
					  mobileCameraMatrix,
					  cv::noArray(),
					  reprojectedPoints);

	double reprojError = 0;
	// calculate reprojection error mean and draw reprojections
	for (unsigned int i = 0; i < inliers.size(); i++)
	{
		Point2f pix = points2d.at(inliers.at(i));
		Point2f reprojPix = reprojectedPoints.at(inliers.at(i));
		reprojError += hypot(pix.x - reprojPix.x, pix.y - reprojPix.y) / inliers.size();
	}

	ROS_INFO_STREAM("inliers reprojection error = " << reprojError);

	return reprojError;
}

/**
 * Esxtracts ORB features from the provided image. Using the parameters set in the related member
 * variables
 *
 * @param       image       The image to use
 * @param[out]  keypoints   The keypoint will be returned here
 * @param[out]  descriptors The descriptors will be returned here
 * @return             0 on success, a negatove value on fail
 */
int CameraPoseEstimator::computeOrbFeatures(const cv::Mat &image,
											std::vector<cv::KeyPoint> &keypoints,
											cv::Mat &descriptors)
{
	keypoints.clear();
	cv::Ptr<cv::ORB> orb;

	orb = cv::ORB::create(orbMaxPoints, orbScaleFactor, orbLevelsNumber);

	orb->detect(image, keypoints);
	if (keypoints.empty())
	{
		ROS_ERROR("No keypoints found");
		return -1;
	}

	orb->compute(image, keypoints, descriptors);
	if (descriptors.empty())
	{
		ROS_ERROR("No descriptors");
		return -2;
	}
	return 0;
}

/**
 * Finds matches between ORB descriptors. Using the parameters set in the related member
 * @param  arcoreDescriptors Descriptors for the mobile camera
 * @param  kinectDescriptors Descriptors for the fixed camera
 * @param  matches           The matches are returned here
 * @return                   0 on success
 */
int CameraPoseEstimator::findOrbMatches(
	const cv::Mat &arcoreDescriptors,
	const cv::Mat &kinectDescriptors,
	std::vector<cv::DMatch> &matches)
{

	// find matches between the descriptors
	std::chrono::steady_clock::time_point beforeMatching = std::chrono::steady_clock::now();
	cv::BFMatcher::create(cv::NORM_HAMMING)->match(arcoreDescriptors, kinectDescriptors, matches); // arcore is query, kinect is trains
	std::chrono::steady_clock::time_point afterMatching = std::chrono::steady_clock::now();
	unsigned long matchingDuration = std::chrono::duration_cast<std::chrono::milliseconds>(afterMatching - beforeMatching).count();
	ROS_DEBUG("Descriptors matching took %lu ms", matchingDuration);
	return 0;
}

/**
 * Filters the provided matches according to related member variables
 * @param  matches     Mathces to be filtered
 * @param  goodMatches The filtere mathces are returned here
 * @return             0 on success
 */
int CameraPoseEstimator::filterMatches(const std::vector<cv::DMatch> &matches, std::vector<cv::DMatch> &goodMatches,
									   const std::vector<cv::KeyPoint> &arcoreKeypoints,
									   const std::vector<cv::KeyPoint> &fixedKeypoints)
{
	double max_dist = std::numeric_limits<double>::min();
	double min_dist = std::numeric_limits<double>::max();
	// Find minimum and maximum distances between matches
	for (unsigned int i = 0; i < matches.size(); i++)
	{
		double dist = matches[i].distance;
		if (dist < min_dist)
		{
			min_dist = dist;
		}
		if (dist > max_dist)
			max_dist = dist;
	}
	ROS_INFO_STREAM("Best/Worst matches = " << min_dist << "/" << max_dist);

	// Filter the matches to keep just the best ones
	for (unsigned int i = 0; i < matches.size(); i++)
	{
		if (matches[i].distance <= matchingThreshold /*std::min(min_dist+(max_dist-min_dist)*0.2,25.0)*/)
		{
			goodMatches.push_back(matches[i]);
			// ROS_INFO_STREAM("got a good match, dist="<<matches[i].distance);
		}
	}

	// Merge matches that link the same two points
	// Remove contradicting matches
	for (size_t i = 0; i < goodMatches.size(); i++)
	{
		cv::KeyPoint arcoreKeypoint1 = arcoreKeypoints.at(goodMatches.at(i).queryIdx);
		std::vector<int> matchesWithSameOrigin;
		for (size_t j = i; j < goodMatches.size(); j++)
		{
			cv::KeyPoint arcoreKeypoint2 = arcoreKeypoints.at(goodMatches.at(j).queryIdx);
			if (cv::norm(arcoreKeypoint1.pt - arcoreKeypoint2.pt) <= keypointMinDistThreshold)
				matchesWithSameOrigin.push_back(j);
		}
		// ROS_INFO_STREAM("Found "<<matchesWithSameOrigin.size()<< " matches with the same origin");
		if (matchesWithSameOrigin.size() <= 1)
			continue;
		bool haveSameDestination = true;
		cv::KeyPoint fixedKeypoint1 = fixedKeypoints.at(goodMatches.at(i).trainIdx);
		for (int mwso : matchesWithSameOrigin)
		{
			// ROS_INFO_STREAM("One is "<<mwso);
			cv::KeyPoint fixedKeypoint2 = fixedKeypoints.at(goodMatches.at(mwso).trainIdx);
			if (cv::norm(fixedKeypoint1.pt - fixedKeypoint2.pt) > keypointMinDistThreshold)
			{
				haveSameDestination = false;
				break;
			}
		}
		// ROS_INFO_STREAM("haveSameDestination = "<<haveSameDestination);

		if (!haveSameDestination)
		{
			// remove all of them
			int elementsErased = 0;
			for (int mwso : matchesWithSameOrigin)
			{
				goodMatches.erase(goodMatches.begin() + mwso - elementsErased);
				elementsErased++;
			}
		}
		else
		{
			// merge them in one
			float averageDist = 0;
			for (int mwso : matchesWithSameOrigin)
				averageDist += goodMatches.at(mwso).distance;
			averageDist /= matchesWithSameOrigin.size();
			goodMatches.at(i).distance = averageDist;
			int elementsErased = 0;
			for (size_t j = 1; j < matchesWithSameOrigin.size(); j++)
			{
				// ROS_INFO_STREAM("Removing "<<matchesWithSameOrigin.at(j)<<". That now is "<<( matchesWithSameOrigin.at(j) - elementsErased));
				goodMatches.erase(goodMatches.begin() + matchesWithSameOrigin.at(j) - elementsErased);
				// ROS_INFO("removed");
				elementsErased++;
			}
		}
	}

	/*
		//take best 4
		std::vector<int> goodMatchesIdx;
		for(int i=0;i<4;i++)
		{
			double bestDist = 10000000;
			int bestMatchIdx = 0;
			for(unsigned int j=0;j<matches.size();j++)
			{
				double dist = matches[j].distance;
				if( dist < bestDist && std::find(goodMatchesIdx.begin(), goodMatchesIdx.end(), j) == goodMatchesIdx.end())//if it is better and it is not in goodMatches
				{
					bestDist = dist;
					bestMatchIdx = j;
				}
			}
			goodMatchesIdx.push_back(bestMatchIdx);
			goodMatches.push_back(matches.at(bestMatchIdx));
		}
		*/
	// goodMatches.push_back(*bestMatch);
	return 0;
}

std::string type2str(int type)
{
	std::string r;

	uchar depth = type & CV_MAT_DEPTH_MASK;
	uchar chans = 1 + (type >> CV_CN_SHIFT);

	switch (depth)
	{
	case CV_8U:
		r = "8U";
		break;
	case CV_8S:
		r = "8S";
		break;
	case CV_16U:
		r = "16U";
		break;
	case CV_16S:
		r = "16S";
		break;
	case CV_32S:
		r = "32S";
		break;
	case CV_32F:
		r = "32F";
		break;
	case CV_64F:
		r = "64F";
		break;
	default:
		r = "User";
		break;
	}

	r += "C";
	r += (chans + '0');

	return r;
}

/**
 * Reads the input messages for a featuresCallback and extracts the data from them
 * @param  arcoreInputMsg                     The input message from the mobile camera
 * @param  kinectInputCameraMsg               Regular mono image message from the fixed camera
 * @param  kinectInputDepthMsg                Depth image message from the fixed camera
 * @param  kinectCameraInfo                   Camera info message for the fixed cmaera
 * @param  arcoreCameraMatrix[out]            The camera matrix for the mobile camera is returned here
 * @param  arcoreDescriptors[out]             The precomputed descriptors from the mobiel camera are returned here
 * @param  arcoreKeypoints[out]               The precomputed keypoints from the mobiel camera are returned here
 * @param  arcoreImageSize[out]               The mobile camera image size is returned here
 * @param  kinectCameraMatrix[out]            The camera matrix for the fixed camera is returned here
 * @param  kinectCameraImg[out]               The regular mono image from the fixed camera is returned here
 * @param  kinectDepthImg[out]                The depth image from the fixed camera is returned here
 * @param  debugArcoreImage[out]              The debug image from the mobile amera is retuend here
 * @return                                    0 on success, a negative value on fail
 */
int CameraPoseEstimator::readReceivedMessages_features(const opt_msgs::ArcoreCameraFeaturesConstPtr &arcoreInputMsg,
													   const sensor_msgs::ImageConstPtr &kinectInputCameraMsg,
													   const sensor_msgs::ImageConstPtr &kinectInputDepthMsg,
													   const sensor_msgs::CameraInfo &kinectCameraInfo,
													   cv::Mat &arcoreCameraMatrix,
													   cv::Mat &arcoreDescriptors,
													   std::vector<cv::KeyPoint> &arcoreKeypoints,
													   cv::Size &arcoreImageSize,
													   cv::Mat &kinectCameraMatrix,
													   cv::Mat &kinectCameraImg,
													   cv::Mat &kinectDepthImg,
													   cv::Mat &debugArcoreImage)
{
	PH_START();
	// std::chrono::steady_clock::time_point beginning = std::chrono::steady_clock::now();

	// convert the camera matrix in a cv::Mat
	arcoreCameraMatrix = cv::Mat(3, 3, CV_64FC1);
	arcoreCameraMatrix.at<double>(0, 0) = arcoreInputMsg->focal_length_x_px;
	arcoreCameraMatrix.at<double>(0, 1) = 0;
	arcoreCameraMatrix.at<double>(0, 2) = arcoreInputMsg->principal_point_x_px;
	arcoreCameraMatrix.at<double>(1, 0) = 0;
	arcoreCameraMatrix.at<double>(1, 1) = arcoreInputMsg->focal_length_y_px;
	arcoreCameraMatrix.at<double>(1, 2) = arcoreInputMsg->principal_point_y_px;
	arcoreCameraMatrix.at<double>(2, 0) = 0;
	arcoreCameraMatrix.at<double>(2, 1) = 0;
	arcoreCameraMatrix.at<double>(2, 2) = 1;

	ROS_DEBUG_STREAM("mobile device camera matrix" << arcoreCameraMatrix);

	kinectCameraMatrix = cv::Mat(3, 3, CV_64FC1);
	kinectCameraMatrix.at<double>(0, 0) = kinectCameraInfo.P[4 * 0 + 0];
	kinectCameraMatrix.at<double>(0, 1) = kinectCameraInfo.P[4 * 0 + 1];
	kinectCameraMatrix.at<double>(0, 2) = kinectCameraInfo.P[4 * 0 + 2];
	kinectCameraMatrix.at<double>(1, 0) = kinectCameraInfo.P[4 * 1 + 0];
	kinectCameraMatrix.at<double>(1, 1) = kinectCameraInfo.P[4 * 1 + 1];
	kinectCameraMatrix.at<double>(1, 2) = kinectCameraInfo.P[4 * 1 + 2];
	kinectCameraMatrix.at<double>(2, 0) = kinectCameraInfo.P[4 * 2 + 0];
	kinectCameraMatrix.at<double>(2, 1) = kinectCameraInfo.P[4 * 2 + 1];
	kinectCameraMatrix.at<double>(2, 2) = kinectCameraInfo.P[4 * 2 + 2];

	ROS_DEBUG_STREAM("static camera matrix " << kinectCameraMatrix);

	arcoreImageSize.width = arcoreInputMsg->image_width_px;
	arcoreImageSize.height = arcoreInputMsg->image_height_px;

	for (unsigned int i = 0; i < arcoreInputMsg->keypoints.size(); i++)
	{
		arcoreKeypoints.push_back(cv::KeyPoint(cv::Point2f(arcoreInputMsg->keypoints[i].x_pos, arcoreInputMsg->keypoints[i].y_pos),
											   arcoreInputMsg->keypoints[i].size,
											   arcoreInputMsg->keypoints[i].angle,
											   arcoreInputMsg->keypoints[i].response,
											   arcoreInputMsg->keypoints[i].octave,
											   arcoreInputMsg->keypoints[i].class_id));
	}

	cv::Mat receivedDescriptors(arcoreInputMsg->descriptors_mat_rows, arcoreInputMsg->descriptors_mat_cols,
								arcoreInputMsg->descriptors_mat_type, (void *)&(arcoreInputMsg->descriptors_mat_data)[0]);
	arcoreDescriptors = receivedDescriptors;

	// decode arcore image if present
	if (arcoreInputMsg->image.data.size() > 0)
	{
		debugArcoreImage = cv_bridge::toCvCopy(arcoreInputMsg->image)->image; // convert compressed image data to cv::Mat

		if (!debugArcoreImage.data)
		{
			ROS_ERROR("couldn't decode arcore image");
			return -2;
		}
		if (debugArcoreImage.channels() == 3)
		{
			// The image sent by the Android app is monochrome, but it is stored in a 3-channel PNG image as the red channel
			// So we extract the red channel and use that.
			// Also the image is flipped on the y axis
			// take red channel
			cv::Mat planes[3];
			split(debugArcoreImage, planes); // planes[2] is the red channel
			debugArcoreImage = planes[2];
			cv::Mat flippedArcoreImg;
			// cv::flip(debugArcoreImage,flippedArcoreImg,0);
			// debugArcoreImage=flippedArcoreImg;
		}
		else if (debugArcoreImage.channels() == 1)
		{
			cv::Mat flippedArcoreImg;
			// cv::flip(debugArcoreImage,flippedArcoreImg,0);
			// debugArcoreImage=flippedArcoreImg;
		}
		else
		{
			ROS_ERROR("received an invalid image, should have either one or three channels");
			return -3;
		}
		// the debug image is not at full resolution, scale it up
		if (debugArcoreImage.cols != arcoreImageSize.width || debugArcoreImage.rows != arcoreImageSize.height)
		{
			cv::Mat scaledMat;
			cv::resize(debugArcoreImage, scaledMat, arcoreImageSize, CV_INTER_LINEAR); // resize image
			debugArcoreImage = scaledMat;
		}
	}

	// decode kinect rgb image
	kinectCameraImg = cv_bridge::toCvShare(kinectInputCameraMsg)->image; // convert compressed image data to cv::Mat
	if (!kinectCameraImg.data)
	{
		ROS_ERROR("couldn't extract kinect camera opencv image");
		return -4;
	}
	ROS_DEBUG("decoded kinect camera image %dx%d type %s",
			  kinectCameraImg.size().width, kinectCameraImg.size().height, type2str(kinectCameraImg.type()).c_str());
	cv::cvtColor(kinectCameraImg, kinectCameraImg, COLOR_BGR2GRAY);
	ROS_DEBUG("converted kinect camera image type %s",
			  type2str(kinectCameraImg.type()).c_str());
	// cv::equalizeHist(kinectCameraImg,kinectCameraImg);
	// cv::imshow("StaticCameraMono", kinectCameraImg);

	// decode kinect depth image
	kinectDepthImg = cv_bridge::toCvShare(kinectInputDepthMsg, "16UC1")->image; // convert compressed image data to cv::Mat
	if (!kinectDepthImg.data)
	{
		ROS_ERROR("couldn't extract kinect depth opencv image");
		return -5;
	}
	ROS_DEBUG("decoded kinect depth image");
	//    cv::namedWindow("StaticCameraDepth", cv::WINDOW_NORMAL);
	// 	cv::resizeWindow("StaticCameraDepth",1280,720);
	//     cv::imshow("StaticCameraDepth", kinectDepthImg);

	// std::chrono::steady_clock::time_point afterDecoding = std::chrono::steady_clock::now();
	// unsigned long decodingDuration = std::chrono::duration_cast<std::chrono::milliseconds>(afterDecoding - beginning).count();
	// ROS_DEBUG("Images decoding and initialization took %lu ms", decodingDuration);
	ROS_DEBUG("Images decoding and initialization took %.3f ms", PH_TOTAL());
	return 0;
}

/**
 * Fixes the matches' depth by doing this for every match:
 *
 * fixes the depth of the provided point to be equal to the lowest depth present
 * in its surroundings. This is useful if you want to get the depthof a foreground
 * object knowing a point close to it.
 *
 * The new depth will be written in the actual provided image. TODO: change this,
 * it's ugly, very ugly.
 *
 * more specifically the function first searches for the closest non-zero pixel,
 * then it searches for the lowest-value pixel in a 10px ring with an inner radius
 * equal to the distance if the previously found pixel
 *
 * @param  inputMatches   Matches to check
 * @param  fixedKeypoints Keypoints on the fixed camera
 * @param  kinectDepthImg Fixed camera depth image
 * @param  outputMatches  The fixed matches are returned here
 * @return                0 on success
 */
int CameraPoseEstimator::fixMatchesDepthOrDrop(const std::vector<cv::DMatch> &inputMatches, const std::vector<cv::KeyPoint> &fixedKeypoints, cv::Mat &kinectDepthImg, std::vector<cv::DMatch> &outputMatches)
{
	outputMatches.clear();
	for (unsigned int i = 0; i < inputMatches.size(); i++)
	{
		// QueryIdx is for arcore descriptors, TrainIdx is for kinect. This is because of how we passed the arguments to BFMatcher::match
		cv::Point2f imgPos = fixedKeypoints.at(inputMatches.at(i).trainIdx).pt;
		// try to find the depth using the closest pixel
		// if(kinectDepthImg.at<uint16_t>(imgPos)==0)
		//{
		Point2i nnz = findNearestNonZeroPixel(kinectDepthImg, imgPos.x, imgPos.y, 100);
		double nnzDist = hypot(nnz.x - imgPos.x, nnz.y - imgPos.y);
		nnz = findLowestNonZeroInRing(kinectDepthImg, imgPos.x, imgPos.y, nnzDist + 10, nnzDist);

		// ROS_INFO("Got closest non-zero pixel, %d;%d",nnz.x,nnz.y);
		kinectDepthImg.at<uint16_t>(imgPos) = kinectDepthImg.at<uint16_t>(nnz);
		//}

		if (kinectDepthImg.at<uint16_t>(imgPos) == 0)
		{
			// ROS_INFO("Dropped match as it had zero depth");
		}
		else
		{
			outputMatches.push_back(inputMatches.at(i));
		}
	}
	return 0;
}

/**
 * Gets the 3d position of the matches and also their 2d position on the mobile camera image
 * @param  inputMatches            The matches
 * @param  fixedKeypoints          The keypoints on the fixed camera side
 * @param  arcoreKeypoints         The keypoints on the mobile camera side
 * @param  kinectDepthImg          The depth image from the fixed camera
 * @param  kinectCameraMatrix      The cmaera matrix for the fixed camera
 * @param  matches3dPos[out]       The 3d positions of the matches are returned here
 * @param  matchesImgPos[out]      The 2d pixel positions of the matches on the mobile camera
 *                                 image are returned here
 * @return                         0 on success
 */
int CameraPoseEstimator::get3dPositionsAndImagePositions(const std::vector<cv::DMatch> &inputMatches,
														 const std::vector<cv::KeyPoint> &fixedKeypoints,
														 const std::vector<cv::KeyPoint> &arcoreKeypoints,
														 const cv::Mat &kinectDepthImg,
														 const cv::Mat &kinectCameraMatrix,
														 std::vector<cv::Point3f> &matches3dPos,
														 std::vector<cv::Point2f> &matchesImgPos)
{
	matches3dPos.clear();
	matchesImgPos.clear();
	for (unsigned int i = 0; i < inputMatches.size(); i++)
	{
		// QueryIdx is for arcore descriptors, TrainIdx is for kinect. This is because of how we passed the arguments to BFMatcher::match
		cv::Point2f kinectPixelPos = fixedKeypoints.at(inputMatches.at(i).trainIdx).pt;
		cv::Point2f arcorePixelPos = arcoreKeypoints.at(inputMatches.at(i).queryIdx).pt;
		cv::Point3f pos3d = get3dPoint(kinectPixelPos.x, kinectPixelPos.y,
									   kinectDepthImg.at<uint16_t>(kinectPixelPos),
									   kinectCameraMatrix.at<double>(0, 0), kinectCameraMatrix.at<double>(1, 1),
									   kinectCameraMatrix.at<double>(0, 2), kinectCameraMatrix.at<double>(1, 2));
		matches3dPos.push_back(pos3d);
		matchesImgPos.push_back(arcorePixelPos);

		// ROS_DEBUG_STREAM("good match between " << kinectPixelPos.x << ";" << kinectPixelPos.y << " \tand \t" << arcorePixelPos.x << ";" << arcorePixelPos.y << " \tdistance = " << inputMatches.at(i).distance);
		// ROS_INFO_STREAM("depth = "<<kinectDepthImg.at<uint16_t>(kinectPixelPos));
	}
	return 0;
}

/**
 * Gets the number of matches used in the last estimation
 * @return The number of matches
 */
int CameraPoseEstimator::getLastEstimateMatchesNumber()
{
	return lastEstimateMatchesNumber;
}

/**
 * Gets the reprojection error of the last estimate
 * @return The reprojection error
 */
double CameraPoseEstimator::getLastEstimateReprojectionError()
{
	return lastEstimateReprojectionError;
}

/**
 * Gets the last pose estimate
 * @return The estimate
 */
geometry_msgs::PoseStamped CameraPoseEstimator::getLastPoseEstimate()
{
	return lastPoseEstimate;
}

/**
 * Gets the device ID of the AR device of which we are estimating the registration
 * @return [description]
 */
string CameraPoseEstimator::getARDeviceId()
{
	return ARDeviceId;
}

/**
 * Tells if this estimator has ever computed an estimate successfully
 * @return did it?
 */
bool CameraPoseEstimator::hasEstimate()
{
	return didComputeEstimate;
}
