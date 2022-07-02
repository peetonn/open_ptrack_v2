OPTAR
======

OPTAR is an OpenPTrack module for integrating OpenPTrack with Augmented Reality
applications.
This module's objective is to provide the coordinate system registration between
the ROS OpenPTrack system and the coordinate systems of the single AR devices.
The single AR devices must be running an application that uses OPTAR.



## Usage
First, launch the rosbridge websocket from the master node:

`roslaunch rosbridge_server rosbridge_websocket.launch`

To use OPTAR you will need to run a ROS node for each fixed camera you want to
use and a single central node. You can decide to not use all of the fixed cameras
but this will negatively affect performance.
It is best to run the nodes related to a specific camera on the computer connected
to that specific camera. This is to minimize the amount of data transfered over
the network.

To launch the camera nodes you can use the 'registration_single_camera_raw.launch'
launcher, you will need to specify the camera sensor name. For example, if you want
to use a camera called 'kinect01', you may run:

    roslaunch optar registration_single_camera_raw.launch sensor_name:=kinect01


To launch the central aggregator node you can instead use the 'registration_estimator.launch'
launch file. This launch file will also launch a node that provides a simple ntp-like
server to synchronize the AR devices and a node that publishes the ARDevices poses.
You don't need to specify any parameter.

    roslaunch optar registration_estimator.launch


If you want to tune the parameters you can use the dynamic_reconfigure package and
change the parameters values for each running node. You can find it in rqt under
' Plugins>Configuration>Dynamic Reconfigure ' or you can launch 'rosrun rqt_reconfigure rqt_reconfigure'



## A Practical Example

If you have a system with two cameras called kinect01 and kinect02 you will need
to launch 3 nodes. After launching the websocket:

Run, preferably on the computer connected directly to kinect01:

    roslaunch optar registration_single_camera_raw.launch sensor_name:=kinect01

Run, preferably on the computer connected directly to kinect02:

    roslaunch optar registration_single_camera_raw.launch sensor_name:=kinect02

Run, on any computer:

    roslaunch optar registration_estimator.launch

You can then tune the parameters running rqt


## Parameter Tuning

#### Single Camera nodes
The parameters for the ardevices_pose_estimator_single_camera_raw nodes are those of
the CameraPoseEstimator::setupParameters() method. You can find a description
for each parameter in the method documentation.

#### Aggregator node
In the ardevices_registration_estimator node the handler_no_msg_timeout_secs parameter
controls the length of time after which an AR device that isn't publishing anything
is forgotten.

The other parameters are those of the ARDeviceRegistrationEstimator::setupParameters() method.
You can find a description for each parameter in the method documentation.
It is important to note that changing the ARDeviceRegistrationEstimator::setupParameters() parameters will not affect devices that are already running, but only new devices.

## Authors
* **Carlo Rizzardo** - *no-marker OPTAR, documentation* - [c-rizz](https://github.com/c-rizz)
* **Daniele Dal Degan** - *marker-based OPTAR* - [dalde](https://github.com/dalde)
