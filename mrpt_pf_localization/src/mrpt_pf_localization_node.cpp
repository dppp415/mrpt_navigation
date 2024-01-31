/* +------------------------------------------------------------------------+
   |                             mrpt_navigation                            |
   |                                                                        |
   | Copyright (c) 2014-2024, Individual contributors, see commit authors   |
   | See: https://github.com/mrpt-ros-pkg/mrpt_navigation                   |
   | All rights reserved. Released under BSD 3-Clause license. See LICENSE  |
   +------------------------------------------------------------------------+ */

#include "mrpt_pf_localization_node.h"

#include <mp2p_icp/metricmap.h>
#include <mrpt/maps/COccupancyGridMap2D.h>
#include <mrpt/obs/CObservation2DRangeScan.h>
#include <mrpt/obs/CObservationBeaconRanges.h>
#include <mrpt/obs/CObservationOdometry.h>
#include <mrpt/obs/CObservationPointCloud.h>
#include <mrpt/obs/CObservationRobotPose.h>
#include <mrpt/ros2bridge/laser_scan.h>
#include <mrpt/ros2bridge/map.h>
#include <mrpt/ros2bridge/point_cloud2.h>
#include <mrpt/ros2bridge/pose.h>
#include <mrpt/ros2bridge/time.h>
#include <mrpt/serialization/CSerializable.h>
#include <mrpt/system/COutputLogger.h>
#include <pose_cov_ops/pose_cov_ops.h>

#include <geometry_msgs/msg/pose_array.hpp>
#include <mrpt_msgs_bridge/beacon.hpp>
#include <tf2_geometry_msgs/tf2_geometry_msgs.hpp>

int main(int argc, char** argv)
{
	rclcpp::init(argc, argv);
	auto node = std::make_shared<PFLocalizationNode>();
	rclcpp::spin(std::dynamic_pointer_cast<rclcpp::Node>(node));
	rclcpp::shutdown();
	return 0;
}

PFLocalizationNode::PFLocalizationNode(const rclcpp::NodeOptions& options)
	: rclcpp::Node("mrpt_pf_localization_node", options)
{
	using std::placeholders::_1;

	// Redirect MRPT logger to ROS logger:
	core_.logging_enable_console_output = false;  // No console, go thru ROS
	core_.logRegisterCallback(
		[this](
			std::string_view msg, const mrpt::system::VerbosityLevel level,
			[[maybe_unused]] std::string_view loggerName,
			[[maybe_unused]] const mrpt::Clock::time_point timestamp) {
			switch (level)
			{
				case mrpt::system::LVL_DEBUG:
					RCLCPP_DEBUG_STREAM(this->get_logger(), msg);
					break;
				case mrpt::system::LVL_INFO:
					RCLCPP_INFO_STREAM(this->get_logger(), msg);
					break;
				case mrpt::system::LVL_WARN:
					RCLCPP_WARN_STREAM(this->get_logger(), msg);
					break;
				case mrpt::system::LVL_ERROR:
					RCLCPP_ERROR_STREAM(this->get_logger(), msg);
					break;
				default:
					break;
			};
		});

	// Params:
	// -----------------
	reload_params_from_ros();

	// Create all publishers and subscribers:
	// ------------------------------------------
	sub_init_pose_ = this->create_subscription<
		geometry_msgs::msg::PoseWithCovarianceStamped>(
		nodeParams_.topic_initialpose, 1,
		std::bind(&PFLocalizationNode::callbackInitialpose, this, _1));

	subMap_ = this->create_subscription<mrpt_msgs::msg::GenericObject>(
		nodeParams_.topic_map, 1,
		std::bind(&PFLocalizationNode::callbackMap, this, _1));

	subOdometry_ = this->create_subscription<nav_msgs::msg::Odometry>(
		nodeParams_.topic_odometry, 1,
		std::bind(&PFLocalizationNode::callbackOdometry, this, _1));

	// Subscribe to one or more laser sources:
	size_t numSensors = 0;

	{
		std::vector<std::string> sources;
		mrpt::system::tokenize(
			nodeParams_.topic_sensors_2d_scan, " ,\t\n", sources);
		for (const auto& topic : sources)
		{
			numSensors++;
			subs_2dlaser_.push_back(
				this->create_subscription<sensor_msgs::msg::LaserScan>(
					topic, 1,
					[topic, this](const sensor_msgs::msg::LaserScan& msg) {
						callbackLaser(msg, topic);
					}));
		}
	}
	{
		std::vector<std::string> sources;
		mrpt::system::tokenize(
			nodeParams_.topic_sensors_point_clouds, " ,\t\n", sources);
		for (const auto& topic : sources)
		{
			numSensors++;
			subs_point_clouds_.push_back(
				this->create_subscription<sensor_msgs::msg::PointCloud2>(
					topic, 1,
					[topic, this](const sensor_msgs::msg::PointCloud2& msg) {
						callbackPointCloud(msg, topic);
					}));
		}
	}

	ASSERTMSG_(
		numSensors > 0,
		"At least one sensor input source must be defined! Refer to the "
		"package documentation.");

	pubParticles_ = this->create_publisher<geometry_msgs::msg::PoseArray>(
		nodeParams_.pub_topic_particles, 1);

	pubPose_ =
		this->create_publisher<geometry_msgs::msg::PoseWithCovarianceStamped>(
			nodeParams_.pub_topic_pose, 1);

#if 0
		else if (sources[i].find("beacon") != std::string::npos)
		{
			sub_sensors_[i] = subscribe(
				sources[i], 1, &PFLocalizationNode::callbackBeacon, this);
		}
		else
		{
			sub_sensors_[i] = subscribe(
				sources[i], 1, &PFLocalizationNode::callbackRobotPose, this);
		}


	// On params change, reload all params:
	// -----------------------------------------
	// Trigger on change -> call:
#endif

	// Create the tf2 buffer and listener
	// ----------------------------------------
	tf_buffer_ = std::make_shared<tf2_ros::Buffer>(this->get_clock());
	tf_listener_ = std::make_shared<tf2_ros::TransformListener>(*tf_buffer_);

	tf_broadcaster_ = std::make_shared<tf2_ros::TransformBroadcaster>(*this);

	// Create timer:
	// ------------------------------------------
	timer_ = this->create_wall_timer(
		std::chrono::microseconds(mrpt::round(1.0e6 / nodeParams_.rate_hz)),
		[this]() { this->loop(); });
}

PFLocalizationNode::~PFLocalizationNode()
{
	// end
}

void PFLocalizationNode::reload_params_from_ros()
{
	// Use MRPT library the same log level as on ROS nodes (only for
	// MRPT_VERSION >= 0x150)
	useROSLogLevel();

	// Unify all ROS params into a in-memory YAML block and pass it to the core
	// object:
	mrpt::containers::yaml paramsBlock = mrpt::containers::yaml::Map();

	const auto& paramsIf = this->get_node_parameters_interface();
	const auto& allParams = paramsIf->get_parameter_overrides();

	for (const auto& kv : allParams)
	{
		// Get param name:
		std::string name = kv.first;

		// ROS2 param names may be nested. Convert that back into YAML nodes:
		// e.g. "foo.bar" -> ["foo"]["bar"].
		mrpt::containers::yaml::map_t* targetYamlNode =
			&paramsBlock.node().asMap();

		for (auto pos = name.find("."); pos != std::string::npos;
			 pos = name.find("."))
		{
			// Split:
			const std::string parentKey = name.substr(0, pos);
			const std::string childKey = name.substr(pos + 1);
			name = childKey;

			// Use subnode:
			if (auto it = targetYamlNode->find(parentKey);
				it == targetYamlNode->end())
			{  // create new:
				(*targetYamlNode)[parentKey] = mrpt::containers::yaml::Map();
				targetYamlNode = &(*targetYamlNode)[parentKey].asMap();
			}
			else
			{  // reuse
				targetYamlNode = &it->second.asMap();
			}
		}

		// Get param value:
		switch (kv.second.get_type())
		{
			case rclcpp::ParameterType::PARAMETER_BOOL:
				(*targetYamlNode)[name] = kv.second.get<bool>();
				break;
			case rclcpp::ParameterType::PARAMETER_DOUBLE:
				(*targetYamlNode)[name] = kv.second.get<double>();
				break;
			case rclcpp::ParameterType::PARAMETER_INTEGER:
				(*targetYamlNode)[name] = kv.second.get<int>();
				break;
			case rclcpp::ParameterType::PARAMETER_STRING:
				(*targetYamlNode)[name] = kv.second.get<std::string>();
				break;
			default:
				RCLCPP_WARN(
					get_logger(), "ROS2 parameter not handled: '%s'",
					name.c_str());
				break;
		}
	}

	core_.init_from_yaml(paramsBlock);
	nodeParams_.loadFrom(paramsBlock);
}

void PFLocalizationNode::loop()
{
	RCLCPP_DEBUG(get_logger(), "loop");

	// PF algorithm:
	core_.step();

	// Publish to ROS:
	if (isTimeFor(nodeParams_.publish_particles_decimation))  //
		publishParticles();

	publishTF();
	publishPose();

	MRPT_TODO("pub quality metrics");

	loopCount_++;
}

bool PFLocalizationNode::waitForTransform(
	mrpt::poses::CPose3D& des, const std::string& target_frame,
	const std::string& source_frame, const int timeoutMilliseconds)
{
	const rclcpp::Duration timeout(0, 1000 * timeoutMilliseconds);
	try
	{
		geometry_msgs::msg::TransformStamped ref_to_trgFrame =
			tf_buffer_->lookupTransform(
				target_frame, source_frame, tf2::TimePointZero,
				tf2::durationFromSec(timeout.seconds()));

		tf2::Transform tf;
		tf2::fromMsg(ref_to_trgFrame.transform, tf);
		des = mrpt::ros2bridge::fromROS(tf);

		RCLCPP_DEBUG(
			get_logger(), "[waitForTransform] Found pose %s -> %s: %s",
			source_frame.c_str(), target_frame.c_str(), des.asString().c_str());

		return true;
	}
	catch (const tf2::TransformException& ex)
	{
		RCLCPP_ERROR(get_logger(), "%s", ex.what());
		return false;
	}
}

void PFLocalizationNode::callbackLaser(
	const sensor_msgs::msg::LaserScan& msg, const std::string& topicName)
{
	RCLCPP_DEBUG(get_logger(), "Received 2D scan (%s)", topicName.c_str());

	// get sensor pose on the robot:
	mrpt::poses::CPose3D sensorPose;
	waitForTransform(
		sensorPose, msg.header.frame_id, nodeParams_.base_footprint_frame_id);

	auto obs = mrpt::obs::CObservation2DRangeScan::Create();
	mrpt::ros2bridge::fromROS(msg, sensorPose, *obs);

	obs->sensorLabel = topicName;

	core_.on_observation(obs);
}

void PFLocalizationNode::callbackPointCloud(
	const sensor_msgs::msg::PointCloud2& msg, const std::string& topicName)
{
	RCLCPP_DEBUG(get_logger(), "Received point cloud (%s)", topicName.c_str());

	// get sensor pose on the robot:
	mrpt::poses::CPose3D sensorPose;
	waitForTransform(
		sensorPose, msg.header.frame_id, nodeParams_.base_footprint_frame_id);

	auto obs = mrpt::obs::CObservationPointCloud::Create();
	obs->sensorLabel = topicName;
	auto pts = mrpt::maps::CSimplePointsMap::Create();
	obs->pointcloud = pts;
	mrpt::ros2bridge::fromROS(msg, *pts);

	obs->sensorLabel = topicName;

	core_.on_observation(obs);
}

void PFLocalizationNode::callbackBeacon(
	const mrpt_msgs::msg::ObservationRangeBeacon& _msg)
{
#if 0
	using namespace mrpt::maps;
	using namespace mrpt::obs;

	time_last_input_ = ros::Time::now();

	// MRPT_LOG_INFO_FMT("callbackBeacon");
	auto beacon = CObservationBeaconRanges::Create();
	// printf("callbackBeacon %s\n", _msg.header.frame_id.c_str());
	if (beacon_poses_.find(_msg.header.frame_id) == beacon_poses_.end())
	{
		updateSensorPose(_msg.header.frame_id);
	}
	else if (state_ != IDLE)  // updating filter; we must be moving or
	// update_while_stopped set to true
	{
		if (param()->update_sensor_pose)
		{
			updateSensorPose(_msg.header.frame_id);
		}
		// mrpt::poses::CPose3D pose = beacon_poses_[_msg.header.frame_id];
		// MRPT_LOG_INFO_FMT("BEACON POSE %4.3f, %4.3f, %4.3f, %4.3f, %4.3f,
		// %4.3f", pose.x(), pose.y(), pose.z(), pose.roll(), pose.pitch(),
		// pose.yaw());
		mrpt_msgs_bridge::fromROS(
			_msg, beacon_poses_[_msg.header.frame_id], *beacon);

		auto sf = CSensoryFrame::Create();
		CObservationOdometry::Ptr odometry;
		odometryForCallback(odometry, _msg.header);

		CObservation::Ptr obs = CObservation::Ptr(beacon);
		sf->insert(obs);
		observation(sf, odometry);
		if (param()->gui_mrpt) show3DDebug(sf);
	}
#endif
}

void PFLocalizationNode::callbackRobotPose(
	const geometry_msgs::msg::PoseWithCovarianceStamped& _msg)
{
#if 0
	using namespace mrpt::maps;
	using namespace mrpt::obs;

	time_last_input_ = ros::Time::now();

	// Robot pose externally provided; we update filter regardless state_
	// attribute's value, as these
	// corrections are typically independent from robot motion (e.g. inputs from
	// GPS or tracking system)
	// XXX admittedly an arbitrary choice; feel free to open an issue if you
	// think it doesn't make sense

	static std::string base_frame_id = param()->base_frame_id;
	static std::string global_frame_id = param()->global_frame_id;

	geometry_msgs::TransformStamped map_to_obs_tf_msg;
	try
	{
		map_to_obs_tf_msg = tf_buffer_.lookupTransform(
			global_frame_id, _msg.header.frame_id, ros::Time(0.0),
			ros::Duration(0.5));
	}
	catch (const tf2::TransformException& e)
	{
		ROS_WARN(
			"Failed to get transform target_frame (%s) to source_frame (%s): "
			"%s",
			global_frame_id.c_str(), _msg.header.frame_id.c_str(), e.what());
		return;
	}
	tf2::Transform map_to_obs_tf;
	tf2::fromMsg(map_to_obs_tf_msg.transform, map_to_obs_tf);

	// Transform observation into global frame, including covariance. For that,
	// we must first obtain
	// the global frame -> observation frame tf as a Pose msg, as required by
	// pose_cov_ops::compose
	geometry_msgs::Pose map_to_obs_pose;
	tf2::toMsg(map_to_obs_tf, map_to_obs_pose);

	geometry_msgs::PoseWithCovarianceStamped obs_pose_world;
	obs_pose_world.header.stamp = _msg.header.stamp;
	obs_pose_world.header.frame_id = global_frame_id;
	pose_cov_ops::compose(map_to_obs_pose, _msg.pose, obs_pose_world.pose);

	// Ensure the covariance matrix can be inverted (no zeros in the diagonal)
	for (unsigned int i = 0; i < obs_pose_world.pose.covariance.size(); ++i)
	{
		if (i / 6 == i % 6 && obs_pose_world.pose.covariance[i] <= 0.0)
			obs_pose_world.pose.covariance[i] =
				std::numeric_limits<double>().infinity();
	}

	// Covert the received pose into an observation the filter can integrate
	auto feature = CObservationRobotPose::Create();

	feature->sensorLabel = _msg.header.frame_id;
	feature->timestamp = mrpt::ros2bridge::fromROS(_msg.header.stamp);
	feature->pose = mrpt::ros2bridge::fromROS(obs_pose_world.pose);

	auto sf = CSensoryFrame::Create();
	CObservationOdometry::Ptr odometry;
	odometryForCallback(odometry, _msg.header);

	CObservation::Ptr obs = CObservation::Ptr(feature);
	sf->insert(obs);
	observation(sf, odometry);
	if (param()->gui_mrpt) show3DDebug(sf);
#endif
}

void PFLocalizationNode::odometryForCallback(
	CObservationOdometry::Ptr& _odometry,
	const std_msgs::msg::Header& _msg_header)
{
#if 0
	std::string base_frame_id = param()->base_frame_id;
	std::string odom_frame_id = param()->odom_frame_id;
	mrpt::poses::CPose3D poseOdom;
	if (this->waitForTransform(
			poseOdom, odom_frame_id, base_frame_id, _msg_header.stamp,
			ros::Duration(1.0)))
	{
		_odometry = CObservationOdometry::Create();
		_odometry->sensorLabel = odom_frame_id;
		_odometry->hasEncodersInfo = false;
		_odometry->hasVelocities = false;
		_odometry->odometry.x() = poseOdom.x();
		_odometry->odometry.y() = poseOdom.y();
		_odometry->odometry.phi() = poseOdom.yaw();
	}
#endif
}

void PFLocalizationNode::callbackMap(const mrpt_msgs::msg::GenericObject& obj)
{
	RCLCPP_INFO(
		get_logger(), "[callbackMap] Received a metric map via ROS topic");

	mrpt::serialization::CSerializable::Ptr o;
	mrpt::serialization::OctetVectorToObject(obj.data, o);

	ASSERT_(o);
	auto mm = std::dynamic_pointer_cast<mp2p_icp::metric_map_t>(o);
	ASSERTMSG_(
		mm, mrpt::format(
				"Expected incoming map of type mp2p_icp::metric_map_t but it "
				"is '%s'",
				o->GetRuntimeClass()->className));

	RCLCPP_INFO_STREAM(
		get_logger(), "[callbackMap] Map contents: " << mm->contents_summary());

	auto mMap = mrpt::maps::CMultiMetricMap::Create();

	for (const auto& [layerName, layerMap] : mm->layers)
	{
		mMap->maps.push_back(layerMap);

		// TODO: Optionally override the map likelihood params?
	}

	core_.set_map_from_metric_map(mMap);
}

void PFLocalizationNode::updateSensorPose(std::string _frame_id)
{
#if 0
	mrpt::poses::CPose3D pose;

	std::string base_frame_id = param()->base_frame_id;

	geometry_msgs::TransformStamped transformStmp;
	try
	{
		ros::Duration timeout(1.0);

		transformStmp = tf_buffer_.lookupTransform(
			base_frame_id, _frame_id, ros::Time(0), timeout);
	}
	catch (const tf2::TransformException& e)
	{
		ROS_WARN(
			"Failed to get transform target_frame (%s) to source_frame (%s): "
			"%s",
			base_frame_id.c_str(), _frame_id.c_str(), e.what());
		return;
	}
	tf2::Transform transform;
	tf2::fromMsg(transformStmp.transform, transform);

	tf2::Vector3 translation = transform.getOrigin();
	tf2::Quaternion quat = transform.getRotation();
	pose.x() = translation.x();
	pose.y() = translation.y();
	pose.z() = translation.z();
	tf2::Matrix3x3 Rsrc(quat);
	mrpt::math::CMatrixDouble33 Rdes;
	for (int c = 0; c < 3; c++)
	{
		for (int r = 0; r < 3; r++)
		{
			Rdes(r, c) = Rsrc.getRow(r)[c];
		}
	}

	pose.setRotationMatrix(Rdes);
	laser_poses_[_frame_id] = pose;
	beacon_poses_[_frame_id] = pose;
#endif
}

void PFLocalizationNode::callbackInitialpose(
	const geometry_msgs::msg::PoseWithCovarianceStamped& msg)
{
	const geometry_msgs::msg::PoseWithCovariance& pose = msg.pose;

	const auto initial_pose = mrpt::ros2bridge::fromROS(pose);

	RCLCPP_INFO_STREAM(
		get_logger(), "[callbackInitialpose] Received: " << initial_pose);

	// Send to core PF runner:
	core_.relocalize_here(initial_pose);
}

void PFLocalizationNode::callbackOdometry(const nav_msgs::msg::Odometry& msg)
{
	auto obs = mrpt::obs::CObservationOdometry::Create();
	obs->timestamp = mrpt::ros2bridge::fromROS(msg.header.stamp);
	obs->sensorLabel = "odom";

	obs->hasVelocities = true;
	obs->velocityLocal = {
		msg.twist.twist.linear.x, msg.twist.twist.linear.y,
		msg.twist.twist.angular.z};

	// SE(3) -> SE(2):
	obs->odometry =
		mrpt::poses::CPose2D(mrpt::ros2bridge::fromROS(msg.pose.pose));

	core_.on_observation(obs);
}

void PFLocalizationNode::updateMap(const nav_msgs::msg::OccupancyGrid& _msg)
{
#if 0
	ASSERT_(metric_map_->countMapsByClass<COccupancyGridMap2D>());
	mrpt::ros2bridge::fromROS(
		_msg, *metric_map_->mapByClass<COccupancyGridMap2D>());
#endif
}

void PFLocalizationNode::publishParticles()
{
#if 0
	if (pub_particles_.getNumSubscribers() > 0)
	{
		geometry_msgs::PoseArray poseArray;
		poseArray.header.frame_id = param()->global_frame_id;
		poseArray.header.stamp = ros::Time::now();
		poseArray.header.seq = loop_count_;
		poseArray.poses.resize(pdf_.particlesCount());
		for (size_t i = 0; i < pdf_.particlesCount(); i++)
		{
			const auto p = mrpt::math::TPose3D(pdf_.getParticlePose(i));
			poseArray.poses[i] = mrpt::ros2bridge::toROS_Pose(p);
		}
		mrpt::poses::CPose2D p;
		pub_particles_.publish(poseArray);
	}
#endif
}

/**
 * @brief Publish map -> odom tf; as the filter provides map -> base, we
 * multiply it by base -> odom
 */
void PFLocalizationNode::publishTF()
{
#if 0
	static std::string base_frame_id = param()->base_frame_id;
	static std::string odom_frame_id = param()->odom_frame_id;
	static std::string global_frame_id = param()->global_frame_id;

	const mrpt::poses::CPose2D robotPoseFromPF = [this]() {
		return pdf_.getMeanVal();
	}();

	tf2::Transform baseOnMap_tf;
	tf2::fromMsg(mrpt::ros2bridge::toROS_Pose(robotPoseFromPF), baseOnMap_tf);

	ros::Time time_last_update(0.0);
	if (state_ == RUN)
	{
		time_last_update = mrpt::ros2bridge::toROS(time_last_update_);

		// Last update time can be too far in the past if we where not updating
		// filter, due to robot stopped or no
		// observations for a while (we optionally show a warning in the second
		// case)
		// We use time zero if so when getting base -> odom tf to prevent an
		// extrapolation into the past exception
		if ((ros::Time::now() - time_last_update).toSec() >
			param()->no_update_tolerance)
		{
			if ((ros::Time::now() - time_last_input_).toSec() >
				param()->no_inputs_tolerance)
			{
				ROS_WARN_THROTTLE(
					2.0,
					"No observations received for %.2fs (tolerance %.2fs); are "
					"robot sensors working?",
					(ros::Time::now() - time_last_input_).toSec(),
					param()->no_inputs_tolerance);
			}
			else
			{
				MRPT_LOG_DEBUG_FMT_THROTTLE(
					2.0,
					"No filter updates for %.2fs (tolerance %.2fs); probably "
					"robot stopped for a while",
					(ros::Time::now() - time_last_update).toSec(),
					param()->no_update_tolerance);
			}

			time_last_update = ros::Time(0.0);
		}
	}

	tf2::Transform odomOnBase_tf;

	{
		geometry_msgs::TransformStamped transform;
		try
		{
			transform = tf_buffer_.lookupTransform(
				base_frame_id, odom_frame_id, time_last_update,
				ros::Duration(0.1));
		}
		catch (const tf2::TransformException& e)
		{
			ROS_WARN_THROTTLE(
				2.0,
				"Failed to get transform target_frame (%s) to source_frame "
				"(%s): "
				"%s",
				base_frame_id.c_str(), odom_frame_id.c_str(), e.what());
			ROS_WARN_THROTTLE(
				2.0,
				"Ensure that your mobile base driver is broadcasting %s -> %s "
				"tf",
				odom_frame_id.c_str(), base_frame_id.c_str());

			return;
		}
		tf2::Transform tx;
		tf2::fromMsg(transform.transform, tx);
		odomOnBase_tf = tx;
	}

	// We want to send a transform that is good up until a tolerance time so
	// that odom can be used
	ros::Time transform_expiration =
		(time_last_update.isZero() ? ros::Time::now() : time_last_update) +
		ros::Duration(param()->transform_tolerance);

	tf2::Stamped<tf2::Transform> tmp_tf_stamped(
		baseOnMap_tf * odomOnBase_tf, transform_expiration, global_frame_id);

	geometry_msgs::TransformStamped tfGeom = tf2::toMsg(tmp_tf_stamped);
	tfGeom.child_frame_id = odom_frame_id;

	tf_broadcaster_.sendTransform(tfGeom);
#endif
}

/**
 * @brief Publish the current pose of the robot
 **/
void PFLocalizationNode::publishPose()
{
#if 0
	// cov for x, y, phi (meter, meter, radian)
	const auto [cov, mean] = pdf_.getCovarianceAndMean();

	geometry_msgs::PoseWithCovarianceStamped p;

	// Fill in the header
	p.header.frame_id = param()->global_frame_id;
	if (loop_count_ < 10 || state_ == IDLE)
	{
		// on first iterations timestamp differs a lot from ROS time
		p.header.stamp = ros::Time::now();
	}
	else
	{
		p.header.stamp = mrpt::ros2bridge::toROS(time_last_update_);
	}

	// Copy in the pose
	p.pose.pose = mrpt::ros2bridge::toROS_Pose(mean);

	// Copy in the covariance, converting from 3-D to 6-D
	for (int i = 0; i < 3; i++)
	{
		for (int j = 0; j < 3; j++)
		{
			int ros_i = i;
			int ros_j = j;
			if (i == 2 || j == 2)
			{
				ros_i = i == 2 ? 5 : i;
				ros_j = j == 2 ? 5 : j;
			}
			p.pose.covariance[ros_i * 6 + ros_j] = cov(i, j);
		}
	}

	pub_pose_.publish(p);
#endif
}

void PFLocalizationNode::useROSLogLevel()
{
	const auto rosLogLevel =
		rcutils_logging_get_logger_level(get_logger().get_name());

	mrpt::system::VerbosityLevel lvl = core_.getMinLoggingLevel();

	if (rosLogLevel <= RCUTILS_LOG_SEVERITY_DEBUG)	// 10
		lvl = mrpt::system::LVL_DEBUG;
	else if (rosLogLevel <= RCUTILS_LOG_SEVERITY_INFO)	// 20
		lvl = mrpt::system::LVL_INFO;
	else if (rosLogLevel <= RCUTILS_LOG_SEVERITY_WARN)	// 30
		lvl = mrpt::system::LVL_WARN;
	else if (rosLogLevel <= RCUTILS_LOG_SEVERITY_ERROR)	 // 40
		lvl = mrpt::system::LVL_ERROR;

	core_.setVerbosityLevel(lvl);
}

void PFLocalizationNode::NodeParameters::loadFrom(
	const mrpt::containers::yaml& cfg)
{
#if 1
	cfg.printAsYAML();
#endif

	MCP_LOAD_OPT(cfg, rate_hz);
	MCP_LOAD_OPT(cfg, transform_tolerance);
	MCP_LOAD_OPT(cfg, no_update_tolerance);
	MCP_LOAD_OPT(cfg, no_inputs_tolerance);
	MCP_LOAD_OPT(cfg, publish_particles_decimation);

	MCP_LOAD_OPT(cfg, base_footprint_frame_id);
	MCP_LOAD_OPT(cfg, odom_frame_id);
	MCP_LOAD_OPT(cfg, global_frame_id);

	MCP_LOAD_OPT(cfg, topic_map);
	MCP_LOAD_OPT(cfg, topic_initialpose);
	MCP_LOAD_OPT(cfg, topic_odometry);

	MCP_LOAD_OPT(cfg, pub_topic_particles);
	MCP_LOAD_OPT(cfg, pub_topic_pose);

	MCP_LOAD_OPT(cfg, topic_sensors_2d_scan);
	MCP_LOAD_OPT(cfg, topic_sensors_point_clouds);
}
