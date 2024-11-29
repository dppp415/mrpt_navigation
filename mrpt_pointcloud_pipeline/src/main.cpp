#include "rclcpp/rclcpp.hpp"
#include "mrpt_pointcloud_pipeline_component.cpp"

int main(int argc, char** argv)
{
	rclcpp::init(argc, argv);

	auto node = std::make_shared<LocalObstaclesNode>();

	rclcpp::spin(node->get_node_base_interface());

	rclcpp::shutdown();

	return 0;
}