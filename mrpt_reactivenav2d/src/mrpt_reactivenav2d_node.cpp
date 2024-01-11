/* +------------------------------------------------------------------------+
   |                             mrpt_navigation                            |
   |                                                                        |
   | Copyright (c) 2014-2024, Individual contributors, see commit authors   |
   | See: https://github.com/mrpt-ros-pkg/mrpt_navigation                   |
   | All rights reserved. Released under BSD 3-Clause license. See LICENSE  |
   +------------------------------------------------------------------------+ */

#include "mrpt_reactivenav2d/mrpt_reactivenav2d_node.hpp"

#include <cassert>
#include <stdexcept>

using namespace mrpt::nav;
using mrpt::maps::CSimplePointsMap;
using namespace mrpt::system;
using namespace mrpt::config;

/**  Constructor: Inits ROS system */
ReactiveNav2DNode::ReactiveNav2DNode(const rclcpp::NodeOptions& options)
	: Node("mrpt_reactivenav2d", options),
	  m_1st_time_init(false),
	  m_target_allowed_distance(0.40f),
	  m_nav_period(0.100),
	  m_save_nav_log(false),
	  m_reactive_if(*this),
	  m_reactive_nav_engine(m_reactive_if)
{
	// Load params
	read_parameters();

	assert(m_nav_period > 0);

	if (m_cfg_file_reactive.empty())
	{
		RCLCPP_ERROR(
			this->get_logger(),
			"Mandatory param 'cfg_file_reactive' is missing!");
		throw;
	}

	if (!mrpt::system::fileExists(m_cfg_file_reactive))
	{
		RCLCPP_ERROR(
			this->get_logger(), "Config file not found: %s",
			m_cfg_file_reactive.c_str());
		throw;
	}

	m_reactive_nav_engine.enableLogFile(m_save_nav_log);

	// Load reactive config:
	// ----------------------------------------------------
	if (!m_cfg_file_reactive.empty())
	{
		try
		{
			CConfigFile cfgFil(m_cfg_file_reactive);
			m_reactive_nav_engine.loadConfigFile(cfgFil);
		}
		catch (std::exception& e)
		{
			RCLCPP_ERROR(
				this->get_logger(),
				"Exception initializing reactive navigation engine:\n%s",
				e.what());
			throw;
		}
	}
	// load robot shape: (1) default, (2) via params, (3) via topic
	// ----------------------------------------------------------------
	// m_reactive_nav_engine.changeRobotShape();

	// Init this subscriber first so we know asap the desired robot shape,
	// if provided via a topic:
	if (!m_sub_topic_robot_shape.empty())
	{
		m_sub_robot_shape =
			this->create_subscription<geometry_msgs::msg::Polygon>(
				m_sub_topic_robot_shape, 1,
				[this](const geometry_msgs::msg::Polygon::SharedPtr poly) {
					this->on_set_robot_shape(poly);
				});

		RCLCPP_INFO(
			this->get_logger(),
			"Params say robot shape will arrive via topic '%s'... waiting 3 "
			"seconds for it.",
			m_sub_topic_robot_shape.c_str());

		// Use rate object to implement sleep
		rclcpp::Rate rate(1);  // 1 Hz
		for (int i = 0; i < 3; i++)
		{
			rclcpp::spin_some(this->get_node_base_interface());
			rate.sleep();
		}
		RCLCPP_INFO(this->get_logger(), "Wait done.");
	}
	else
	{
		// Load robot shape: 1/2 polygon
		// ---------------------------------------------
		CConfigFile c(m_cfg_file_reactive);
		std::string s = "CReactiveNavigationSystem";

		std::vector<float> xs, ys;
		c.read_vector(
			s, "RobotModel_shape2D_xs", std::vector<float>(), xs, false);
		c.read_vector(
			s, "RobotModel_shape2D_ys", std::vector<float>(), ys, false);
		ASSERTMSG_(
			xs.size() == ys.size(),
			"Config parameters `RobotModel_shape2D_xs` and "
			"`RobotModel_shape2D_ys` "
			"must have the same length!");
		if (!xs.empty())
		{
			mrpt::math::CPolygon poly;
			poly.resize(xs.size());
			for (size_t i = 0; i < xs.size(); i++)
			{
				poly[i].x = xs[i];
				poly[i].y = ys[i];
			}

			std::lock_guard<std::mutex> csl(m_reactive_nav_engine_cs);
			m_reactive_nav_engine.changeRobotShape(poly);
		}

		// Load robot shape: 2/2 circle
		// ---------------------------------------------
		if (const double robot_radius = c.read_double(
				s, "RobotModel_circular_shape_radius", -1.0, false);
			robot_radius > 0)
		{
			std::lock_guard<std::mutex> csl(m_reactive_nav_engine_cs);
			m_reactive_nav_engine.changeRobotCircularShapeRadius(robot_radius);
		}
	}

	// Init ROS publishers:
	// -----------------------
	m_pub_cmd_vel = this->create_publisher<geometry_msgs::msg::Twist>(
		m_pub_topic_cmd_vel, 1);

	// Init ROS subs:
	// -----------------------
	m_sub_odometry = this->create_subscription<nav_msgs::msg::Odometry>(
		m_sub_topic_odometry, 1,
		[this](const nav_msgs::msg::Odometry::SharedPtr odom) {
			this->on_odometry_received(odom);
		});

	m_sub_wp_seq = this->create_subscription<mrpt_msgs::msg::WaypointSequence>(
		m_sub_topic_wp_seq, 1,
		[this](const mrpt_msgs::msg::WaypointSequence::SharedPtr msg) {
			this->on_waypoint_seq_received(msg);
		});

	m_sub_nav_goal = this->create_subscription<geometry_msgs::msg::PoseStamped>(
		m_sub_topic_reactive_nav_goal, 1,
		[this](const geometry_msgs::msg::PoseStamped::SharedPtr msg) {
			this->on_goal_received(msg);
		});

	m_sub_local_obs = this->create_subscription<sensor_msgs::msg::PointCloud2>(
		m_sub_topic_local_obstacles, 1,
		[this](const sensor_msgs::msg::PointCloud2::SharedPtr msg) {
			this->on_local_obstacles(msg);
		});

	// Init tf buffers
	// ----------------------------------------------------
	m_tf_buffer = std::make_shared<tf2_ros::Buffer>(this->get_clock());
	m_tf_listener = std::make_shared<tf2_ros::TransformListener>(*m_tf_buffer);

	// Init timer:
	// ----------------------------------------------------
	m_timer_run_nav = this->create_wall_timer(
		std::chrono::duration<double>(m_nav_period),
		[this]() { this->on_do_navigation(); });

}  // end ctor

void ReactiveNav2DNode::read_parameters()
{
	declare_parameter<std::string>(
		"cfg_file_reactive", "reactive2d_config.ini");
	get_parameter("cfg_file_reactive", m_cfg_file_reactive);
	RCLCPP_INFO(
		this->get_logger(), "cfg_file_reactive %s",
		m_cfg_file_reactive.c_str());

	declare_parameter<double>(
		"target_allowed_distance", m_target_allowed_distance);
	get_parameter("target_allowed_distance", m_target_allowed_distance);
	RCLCPP_INFO(
		this->get_logger(), "target_allowed_distance: %f",
		m_target_allowed_distance);

	declare_parameter<double>("nav_period", m_nav_period);
	get_parameter("nav_period", m_nav_period);
	RCLCPP_INFO(this->get_logger(), "nav_period: %f", m_nav_period);

	declare_parameter<std::string>("frameid_reference", m_frameid_reference);
	get_parameter("frameid_reference", m_frameid_reference);
	RCLCPP_INFO(
		this->get_logger(), "frameid_reference: %s",
		m_frameid_reference.c_str());

	declare_parameter<std::string>("frameid_robot", m_frameid_robot);
	get_parameter("frameid_robot", m_frameid_robot);
	RCLCPP_INFO(
		this->get_logger(), "frameid_robot: %s", m_frameid_robot.c_str());

	declare_parameter<std::string>("topic_wp_seq", m_sub_topic_wp_seq);
	get_parameter("topic_wp_seq", m_sub_topic_wp_seq);
	RCLCPP_INFO(
		this->get_logger(), "topic_wp_seq: %s", m_sub_topic_wp_seq.c_str());

	declare_parameter<std::string>(
		"topic_reactive_nav_goal", m_sub_topic_reactive_nav_goal);
	get_parameter("topic_reactive_nav_goal", m_sub_topic_reactive_nav_goal);
	RCLCPP_INFO(
		this->get_logger(), "topic_reactive_nav_goal: %s",
		m_sub_topic_reactive_nav_goal.c_str());

	declare_parameter<std::string>("topic_odometry", m_sub_topic_odometry);
	get_parameter("topic_odometry", m_sub_topic_odometry);
	RCLCPP_INFO(
		this->get_logger(), "topic_odometry: %s", m_sub_topic_odometry.c_str());

	declare_parameter<std::string>("topic_cmd_vel", m_pub_topic_cmd_vel);
	get_parameter("topic_cmd_vel", m_pub_topic_cmd_vel);
	RCLCPP_INFO(
		this->get_logger(), "topic_cmd_vel: %s", m_pub_topic_cmd_vel.c_str());

	declare_parameter<std::string>(
		"topic_obstacles", m_sub_topic_local_obstacles);
	get_parameter("topic_obstacles", m_sub_topic_local_obstacles);
	RCLCPP_INFO(
		this->get_logger(), "topic_obstacles: %s",
		m_sub_topic_local_obstacles.c_str());

	declare_parameter<std::string>(
		"topic_robot_shape", m_sub_topic_robot_shape);
	get_parameter("topic_robot_shape", m_sub_topic_robot_shape);
	RCLCPP_INFO(
		this->get_logger(), "topic_robot_shape: %s",
		m_sub_topic_robot_shape.c_str());

	declare_parameter<bool>("save_nav_log", false);
	get_parameter("save_nav_log", m_save_nav_log);
	RCLCPP_INFO(
		this->get_logger(), "save_nav_log: %s", m_save_nav_log ? "yes" : "no");

	declare_parameter<std::string>("ptg_plugin_files", "");
	get_parameter("ptg_plugin_files", m_plugin_file);
	RCLCPP_INFO(
		this->get_logger(), "ptg_plugin_files: %s", m_plugin_file.c_str());

	if (!m_plugin_file.empty())
	{
		RCLCPP_INFO_STREAM(
			this->get_logger(), "About to load plugins: " << m_plugin_file);
		std::string errorMsgs;
		if (!mrpt::system::loadPluginModules(m_plugin_file, errorMsgs))
		{
			RCLCPP_ERROR_STREAM(
				this->get_logger(),
				"Error loading rnav plugins: " << errorMsgs);
		}
		RCLCPP_INFO_STREAM(this->get_logger(), "Pluginns loaded OK.");
	}
}

/**
 * @brief Issue a navigation command
 * @param target The target location
 */
void ReactiveNav2DNode::navigate_to(const mrpt::math::TPose2D& target)
{
	RCLCPP_INFO(
		this->get_logger(), "[navigateTo] Starting navigation to %s",
		target.asString().c_str());

	CAbstractPTGBasedReactive::TNavigationParamsPTG navParams;

	CAbstractNavigator::TargetInfo target_info;
	target_info.target_coords.x = target.x;
	target_info.target_coords.y = target.y;
	target_info.targetAllowedDistance = m_target_allowed_distance;
	target_info.targetIsRelative = false;

	// API for single targets:
	navParams.target = target_info;

	// Optional: restrict the PTGs to use
	// navParams.restrict_PTG_indices.push_back(1);

	{
		std::lock_guard<std::mutex> csl(m_reactive_nav_engine_cs);
		m_reactive_nav_engine.navigate(&navParams);
	}
}

/** Callback: On run navigation */
void ReactiveNav2DNode::on_do_navigation()
{
	// 1st time init:
	// ----------------------------------------------------
	if (!m_1st_time_init)
	{
		m_1st_time_init = true;
		RCLCPP_INFO(
			this->get_logger(),
			"[ReactiveNav2DNode] Initializing reactive navigation "
			"engine...");
		{
			std::lock_guard<std::mutex> csl(m_reactive_nav_engine_cs);
			m_reactive_nav_engine.initialize();
		}
		RCLCPP_INFO(
			this->get_logger(),
			"[ReactiveNav2DNode] Reactive navigation engine init done!");
	}

	CTimeLoggerEntry tle(m_profiler, "on_do_navigation");
	// Main nav loop (in whatever state nav is: IDLE, NAVIGATING, etc.)
	m_reactive_nav_engine.navigationStep();
}

void ReactiveNav2DNode::on_odometry_received(
	const nav_msgs::msg::Odometry::SharedPtr& msg)
{
	std::lock_guard<std::mutex> csl(m_odometry_cs);
	tf2::Quaternion quat(
		msg->pose.pose.orientation.x, msg->pose.pose.orientation.y,
		msg->pose.pose.orientation.z, msg->pose.pose.orientation.w);
	tf2::Matrix3x3 mat(quat);
	double roll, pitch, yaw;
	mat.getRPY(roll, pitch, yaw);
	m_odometry.odometry = mrpt::poses::CPose2D(
		msg->pose.pose.position.x, msg->pose.pose.position.y, yaw);

	m_odometry.velocityLocal.vx = msg->twist.twist.linear.x;
	m_odometry.velocityLocal.vy = msg->twist.twist.linear.y;
	m_odometry.velocityLocal.omega = msg->twist.twist.angular.z;
	m_odometry.hasVelocities = true;

	RCLCPP_DEBUG_STREAM(this->get_logger(), "Odometry updated");
}

void ReactiveNav2DNode::on_waypoint_seq_received(
	const mrpt_msgs::msg::WaypointSequence::SharedPtr& wps)
{
	update_waypoint_sequence(std::move(wps));
}

void ReactiveNav2DNode::update_waypoint_sequence(
	const mrpt_msgs::msg::WaypointSequence::SharedPtr& msg)
{
	mrpt::nav::TWaypointSequence wps;

	mrpt::poses::CPose3D relPose = mrpt::poses::CPose3D::Identity();

	// Convert to the "m_frameid_reference" frame of coordinates:
	if (msg->header.frame_id != m_frameid_reference)
		waitForTransform(relPose, m_frameid_reference, msg->header.frame_id);

	for (const auto& wp : msg->waypoints)
	{
		auto trg = mrpt::ros2bridge::fromROS(wp.target);
		trg = relPose + trg;  // local to global frame, if needed.

		auto waypoint = mrpt::nav::TWaypoint(
			trg.x(), trg.y(), wp.allowed_distance, wp.allow_skip);

		// regular number, not NAN
		if (trg.yaw() == trg.yaw() && !wp.ignore_heading)
			waypoint.target_heading = trg.yaw();

		wps.waypoints.push_back(waypoint);
	}

	RCLCPP_INFO_STREAM(this->get_logger(), "New navigateWaypoints() command");
	{
		std::lock_guard<std::mutex> csl(m_reactive_nav_engine_cs);
		m_reactive_nav_engine.navigateWaypoints(wps);
	}
}

void ReactiveNav2DNode::on_goal_received(
	const geometry_msgs::msg::PoseStamped::SharedPtr& trg_ptr)
{
	geometry_msgs::msg::PoseStamped trg = *trg_ptr;

	RCLCPP_INFO(
		this->get_logger(),
		"Nav target received via topic sub: (%.03f,%.03f, %.03fdeg) "
		"[frame_id=%s]",
		trg.pose.position.x, trg.pose.position.y,
		trg.pose.orientation.z * 180.0 / M_PI, trg.header.frame_id.c_str());

	auto trgPose = mrpt::ros2bridge::fromROS(trg.pose);

	// Convert to the "m_frameid_reference" frame of coordinates:
	if (trg.header.frame_id != m_frameid_reference)
	{
		mrpt::poses::CPose3D relPose;
		waitForTransform(
			relPose, m_frameid_reference, trg_ptr->header.frame_id);
		trgPose = relPose + trgPose;
	}

	this->navigate_to(mrpt::poses::CPose2D(trgPose).asTPose());
}

void ReactiveNav2DNode::on_local_obstacles(
	const sensor_msgs::msg::PointCloud2::SharedPtr& obs)
{
	std::lock_guard<std::mutex> csl(m_last_obstacles_cs);
	mrpt::ros2bridge::fromROS(*obs, m_last_obstacles);
	RCLCPP_DEBUG(
		this->get_logger(), "Local obstacles received: %u points",
		static_cast<unsigned int>(m_last_obstacles.size()));
}

void ReactiveNav2DNode::on_set_robot_shape(
	const geometry_msgs::msg::Polygon::SharedPtr& newShape)
{
	RCLCPP_INFO_STREAM(
		this->get_logger(),
		"[onSetRobotShape] Robot shape received via topic:");
	for (const auto& point : newShape->points)
	{
		RCLCPP_INFO_STREAM(
			this->get_logger(), "Point - x: " << point.x << ", y: " << point.y
											  << ", z: " << point.z);
	}

	mrpt::math::CPolygon poly;
	poly.resize(newShape->points.size());
	for (size_t i = 0; i < newShape->points.size(); i++)
	{
		poly[i].x = newShape->points[i].x;
		poly[i].y = newShape->points[i].y;
	}

	{
		std::lock_guard<std::mutex> csl(m_reactive_nav_engine_cs);
		m_reactive_nav_engine.changeRobotShape(poly);
	}
}

bool ReactiveNav2DNode::waitForTransform(
	mrpt::poses::CPose3D& des, const std::string& target_frame,
	const std::string& source_frame, const int timeoutMilliseconds)
{
	const rclcpp::Duration timeout(0, 1000 * timeoutMilliseconds);
	try
	{
		geometry_msgs::msg::TransformStamped ref_to_trgFrame =
			m_tf_buffer->lookupTransform(
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

int main(int argc, char** argv)
{
	rclcpp::init(argc, argv);

	auto node = std::make_shared<ReactiveNav2DNode>();

	rclcpp::spin(node);

	rclcpp::shutdown();

	return 0;
}
