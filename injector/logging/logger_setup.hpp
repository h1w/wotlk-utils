#pragma once

#include <string>

namespace logger
{
	struct Options
	{
		std::string program_name = "app";
		std::string base_filename = "app";
		std::string log_directory = "./logs";
		bool console_enabled = true;
		bool console_use_color = true;
	};

	void Initialize(const Options& opts);
	void Shutdown();
}
