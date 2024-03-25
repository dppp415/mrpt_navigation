/* +------------------------------------------------------------------------+
   |                             mrpt_navigation                            |
   |                                                                        |
   | Copyright (c) 2014-2024, Individual contributors, see commit authors   |
   | See: https://github.com/mrpt-ros-pkg/mrpt_navigation                   |
   | All rights reserved. Released under BSD 3-Clause license. See LICENSE  |
   +------------------------------------------------------------------------+ */

#include <gtest/gtest.h>
#include <mp2p_icp/metricmap.h>
#include <mrpt/containers/yaml.h>
#include <mrpt/core/get_env.h>
#include <mrpt/obs/CRawlog.h>
#include <mrpt_pf_localization/mrpt_pf_localization_core.h>

#include <thread>

const bool RUN_TESTS_WITH_GUI = mrpt::get_env("RUN_TESTS_WITH_GUI", false);

const auto TEST_MM_FILE = mrpt::get_env<std::string>("TEST_MM_FILE", "");

const char* TEST_PARAMS_YAML_FILE =
	MRPT_LOCALIZATION_SOURCE_DIR "/params/default.config.yaml";

const char* TEST_MAP_CONFIG_FILE =
	MRPT_LOCALIZATION_SOURCE_DIR "/params/map-occgrid2d.ini";

const char* TEST_SIMPLEMAP_FILE = MRPT_LOCALIZATION_SOURCE_DIR
	"/../mrpt_tutorials/maps/gh25_simulated.simplemap";

const std::string TEST_RAWLOG_FILE = mrpt::get_env<std::string>(
	"TEST_RAWLOG_FILE", MRPT_LOCALIZATION_SOURCE_DIR
	"/../mrpt_tutorials/datasets/driving_in_office_obs.rawlog");

TEST(PF_Localization, InitState)
{
	PFLocalizationCore loc;

	for (int i = 0; i < 10; i++)
	{
		EXPECT_EQ(loc.getState(), PFLocalizationCore::State::UNINITIALIZED);
		loc.step();
	}
}

TEST(PF_Localization, RunRealDataset)
{
	PFLocalizationCore loc;

	if (mrpt::get_env<bool>("VERBOSE"))
		loc.setMinLoggingLevel(mrpt::system::LVL_DEBUG);

	auto p = mrpt::containers::yaml::FromFile(TEST_PARAMS_YAML_FILE);
	mrpt::containers::yaml params = p["/**"]["ros__parameters"];

	if (!RUN_TESTS_WITH_GUI)
	{
		// For running tests, disable GUI (comment out to see the GUI live):
		params["gui_enable"] = false;
	}

	// Load params:
	loc.init_from_yaml(params);

	// Check params:
	EXPECT_EQ(loc.getParams().initial_particles_per_m2, 10U);

	// Check that we are still in UNINITILIZED state, even after stepping,
	// since we don't have a map yet:
	EXPECT_EQ(loc.getState(), PFLocalizationCore::State::UNINITIALIZED);
	loc.step();
	EXPECT_EQ(loc.getState(), PFLocalizationCore::State::UNINITIALIZED);

	// Now, load a map:
	if (TEST_MM_FILE.empty())
	{
		bool loadOk = loc.set_map_from_simple_map(
			TEST_MAP_CONFIG_FILE, TEST_SIMPLEMAP_FILE);
		EXPECT_TRUE(loadOk);
	}
	else
	{
		mp2p_icp::metric_map_t mm;
		bool loadOk = mm.load_from_file(TEST_MM_FILE);
		EXPECT_TRUE(loadOk);

		loc.set_map_from_metric_map(mm);
	}

	// Now, we should transition to TO_INITIALIZE:
	loc.step();
	EXPECT_EQ(loc.getState(), PFLocalizationCore::State::TO_BE_INITIALIZED);

	// And next iter, we should be running with all particles around:
	loc.step();
	EXPECT_EQ(loc.getState(), PFLocalizationCore::State::RUNNING);

	MRPT_TODO("Check that number of particles is high");

	// Run for a small dataset:
	mrpt::obs::CRawlog dataset;
	dataset.loadFromRawLogFile(TEST_RAWLOG_FILE);
	EXPECT_GT(dataset.size(), 20U);

	double lastStepTime = 0.0;
	for (const auto& observation : dataset)
	{
		const auto obs =
			std::dynamic_pointer_cast<mrpt::obs::CObservation>(observation);

		loc.on_observation(obs);

		const double thisObsTim = mrpt::Clock::toDouble(obs->timestamp);

		if (thisObsTim - lastStepTime > 0.10)
		{
			lastStepTime = thisObsTim;
			loc.step();

			if (loc.getParams().gui_enable)
				std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
	}

	MRPT_TODO("Check PF convergence to ground truth");
}

int main(int argc, char** argv)
{
	::testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
