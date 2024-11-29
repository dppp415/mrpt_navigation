#include "rclcpp/rclcpp.hpp"
#include "mrpt_pf_localization_component.cpp"

int main(int argc, char** argv)
{
	rclcpp::init(argc, argv);
	auto node = std::make_shared<PFLocalizationNode>();
	rclcpp::spin(std::dynamic_pointer_cast<rclcpp::Node>(node));
	rclcpp::shutdown();
	return 0;
}