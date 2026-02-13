#include "logger_setup.hpp"
#include "glog_custom_formatter.hpp"

namespace logger
{
	void Initialize(const Options& opts)
	{
		GlogCustomFormatter::Config cfg;

		// уровень логирования
		cfg.min_log_level = google::GLOG_INFO;
		cfg.verbose_level = 2;

		// консоль
		cfg.console_enabled = opts.console_enabled;
		cfg.console_use_color = opts.console_use_color;
		cfg.console_pattern = "[%L] %T [%F:%N] %M";

		// файл
		cfg.file_enabled = false;
		cfg.log_directory = opts.log_directory;
		cfg.base_filename = opts.base_filename;
		cfg.file_pattern = "%l%T %P %I %F:%N] %M";

		// ротация
		cfg.max_file_size_mb = 50;
		cfg.max_file_count = 5;
		cfg.max_file_age_hours = 168;
		cfg.rotate_daily = true;

		GlogCustomFormatter::Instance().Initialize(cfg, opts.program_name.c_str());
	}

	void Shutdown()
	{
		GlogCustomFormatter::Instance().Shutdown();
	}
}
