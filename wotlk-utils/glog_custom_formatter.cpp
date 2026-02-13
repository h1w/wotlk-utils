#include "glog_custom_formatter.hpp"

// Объявление только нужных WinAPI-функций напрямую,
// <Windows.h> тащит макросы FormatMessage, min, max
extern "C" {
	__declspec(dllimport) int __stdcall SetConsoleOutputCP(unsigned int wCodePageID);
	__declspec(dllimport) int __stdcall SetConsoleCP(unsigned int wCodePageID);
}

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <regex>
#include <sstream>
#include <process.h>

namespace fs = std::filesystem;

// синглтон
GlogCustomFormatter& GlogCustomFormatter::Instance()
{
	static GlogCustomFormatter instance;
	return instance;
}

GlogCustomFormatter::GlogCustomFormatter() = default;
GlogCustomFormatter::~GlogCustomFormatter() { Shutdown(); }

// жизненный цикл
void GlogCustomFormatter::Initialize(const Config& cfg, const char* argv0)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (initialized_) return;

	config_ = cfg;

	// Переключение консоли Windows на UTF-8 для корректного вывода кириллицы
	SetConsoleOutputCP(65001); // CP_UTF8
	SetConsoleCP(65001);
	
	// подготовка директории
	if (config_.file_enabled)
	{
		fs::create_directories(config_.log_directory);
	}

	// настройка glog
	// отключение стандартного вывода glog в stderr / файлы, всё пойдет через CustomSink
	FLAGS_logtostderr = true;
	FLAGS_alsologtostderr = false;
	FLAGS_stderrthreshold = google::NUM_SEVERITIES; // подавляем stderr
	FLAGS_minloglevel = config_.min_log_level;
	FLAGS_v = config_.verbose_level;
	FLAGS_colorlogtostderr = false;

	google::InitGoogleLogging(argv0);

	// отключение записи glog в собственные файлы, собственное управление файлами
	FLAGS_logtostderr = false;
	FLAGS_stderrthreshold = google::NUM_SEVERITIES;
	for (int sev = 0; sev < google::NUM_SEVERITIES; ++sev) {
		google::SetLogDestination(static_cast<google::LogSeverity>(sev), "");
	}

	// регистрация кастомного синка
	sink_ = std::make_unique<CustomSink>(*this);
	google::AddLogSink(sink_.get());

	// открытие лог-файла
	if (config_.file_enabled)
	{
		OpenLogFile();
	}

	// фоновый поток ротации (проверка каждые 30 секунд)
	stop_rotation_ = false;
	rotation_thread_ = std::thread(&GlogCustomFormatter::RotationThreadFunc, this);

	initialized_ = true;
}

void GlogCustomFormatter::Shutdown()
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (!initialized_) return;

	stop_rotation_ = true;
	if (rotation_thread_.joinable()) {
		rotation_thread_.join();
	}

	if (sink_) {
		google::RemoveLogSink(sink_.get());
		sink_.reset();
	}
	google::ShutdownGoogleLogging();

	CloseLogFile();
	initialized_ = false;
}

// изменение параметров на лету
void GlogCustomFormatter::SetMinLogLevel(google::LogSeverity level)
{
	std::lock_guard<std::mutex> lock(mutex_);
	config_.min_log_level = level;
	FLAGS_minloglevel = level;
}

void GlogCustomFormatter::SetVerboseLevel(int level)
{
	std::lock_guard<std::mutex> lock(mutex_);
	config_.verbose_level = level;
	FLAGS_v = level;
}

void GlogCustomFormatter::SetConsoleEnabled(bool enabled)
{
	std::lock_guard<std::mutex> lock(mutex_);
	config_.console_enabled = enabled;
}

void GlogCustomFormatter::SetFileEnabled(bool enabled)
{
	std::lock_guard<std::mutex> lock(mutex_);
	config_.file_enabled = enabled;
	if (enabled && !log_file_)
	{
		OpenLogFile();
	}
	else if (!enabled && log_file_)
	{
		CloseLogFile();
	}
}

void GlogCustomFormatter::SetConsolePattern(const std::string& pattern)
{
	std::lock_guard<std::mutex> lock(mutex_);
	config_.console_pattern = pattern;
}

void GlogCustomFormatter::SetFilePattern(const std::string& pattern)
{
	std::lock_guard<std::mutex> lock(mutex_);
	config_.file_pattern = pattern;
}

void GlogCustomFormatter::RotateNow()
{
	std::lock_guard<std::mutex> lock(mutex_);
	CloseLogFile();
	OpenLogFile();
	EnforceMaxCount();
	EnforceMaxAge();
}

// CustomSink перехват всех сообщений от glog
GlogCustomFormatter::CustomSink::CustomSink(GlogCustomFormatter& owner) : owner_(owner) {}
GlogCustomFormatter::CustomSink::~CustomSink() = default;

void GlogCustomFormatter::CustomSink::send(
	google::LogSeverity severity,
	const char* full_filename,
	const char* base_filename,
	int line,
	const google::LogMessageTime& logmsgtime,
	const char* message,
	std::size_t message_len)
{
	// консольный вывод
	if (owner_.config_.console_enabled)
	{
		std::string formatted = FormatMessage(
			owner_.config_.console_pattern,
			severity,
			base_filename,
			line,
			logmsgtime,
			message,
			message_len
		);

		if (owner_.config_.console_use_color)
		{
			std::cerr << SeverityColorPrefix(severity) << formatted << ColorReset() << "\n";
		}
		else
		{
			std::cerr << formatted << "\n";
		}
	}

	// файловый вывод
	if (owner_.config_.file_enabled)
	{
		std::string formatted = FormatMessage(
			owner_.config_.file_pattern,
			severity,
			base_filename,
			line,
			logmsgtime,
			message,
			message_len
		);

		owner_.WriteToFile(formatted, severity);
	}
}

// форматирование сообщений
std::string GlogCustomFormatter::FormatMessage(
	const std::string& pattern,
	google::LogSeverity severity,
	const char* filename,
	int line,
	const google::LogMessageTime& logmsgtime,
	const char* message,
	std::size_t message_len
)
{
	// формирование метки времени: YYYY-MM-DD HH:MM:SS.microseconds
	char timebuf[64];
	std::snprintf(
		timebuf, sizeof(timebuf),
		"%04d-%02d-%02d %02d:%02d:%02d.%06d",
		logmsgtime.year() + 1900, logmsgtime.month() + 1, logmsgtime.day(),
		logmsgtime.hour(), logmsgtime.min(), logmsgtime.sec(),
		logmsgtime.usec()
	);

	std::string msg_str(message, message_len);

	// подставление плейсхолдеров
	std::string result = pattern;

	auto replace_all = [](std::string& str, const std::string& from, const std::string& to)
	{
		std::size_t pos = 0;
		while ((pos = str.find(from, pos)) != std::string::npos)
		{
			str.replace(pos, from.size(), to);
			pos += to.size();
		}
	};

	replace_all(result, "%L", SeverityToString(severity));
	replace_all(result, "%l", std::string(1, SeverityToChar(severity)));
	replace_all(result, "%T", std::string(timebuf));
	replace_all(result, "%F", std::string(filename ? filename : "unknown"));
	replace_all(result, "%N", std::to_string(line));
	replace_all(result, "%P", std::to_string(static_cast<int>(_getpid())));

	// Thread ID
	std::ostringstream tid;
	tid << std::this_thread::get_id();
	replace_all(result, "%I", tid.str());

	replace_all(result, "%M", msg_str);

	return result;
}

char GlogCustomFormatter::SeverityToChar(google::LogSeverity severity)
{
	switch (severity) {
	case google::GLOG_INFO:    return 'I';
	case google::GLOG_WARNING: return 'W';
	case google::GLOG_ERROR:   return 'E';
	case google::GLOG_FATAL:   return 'F';
	default:                   return '?';
	}
}

const char* GlogCustomFormatter::SeverityToString(google::LogSeverity severity)
{
	switch (severity) {
	case google::GLOG_INFO:    return "INFO";
	case google::GLOG_WARNING: return "WARN";
	case google::GLOG_ERROR:   return "ERROR";
	case google::GLOG_FATAL:   return "FATAL";
	default:                   return "UNKNOWN";
	}
}

std::string GlogCustomFormatter::SeverityColorPrefix(google::LogSeverity severity)
{
	// ANSI escape-коды
	switch (severity) {
	case google::GLOG_INFO:    return "\033[32m";       // зелёный
	case google::GLOG_WARNING: return "\033[33m";       // жёлтый
	case google::GLOG_ERROR:   return "\033[31m";       // красный
	case google::GLOG_FATAL:   return "\033[1;31m";     // жирный красный
	default:                   return "\033[0m";
	}
}

std::string GlogCustomFormatter::ColorReset()
{
	return "\033[0m";
}

// файловый ввод и ротация
void GlogCustomFormatter::WriteToFile(const std::string& formatted, google::LogSeverity /*severity*/)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (!log_file_) return;

	std::string line = formatted + "\n";
	std::fwrite(line.data(), 1, line.size(), log_file_);
	std::fflush(log_file_);
	current_size_ += line.size();

	RotateIfNeeded();
}

std::string GlogCustomFormatter::BuildLogFilePath() const
{
	// Формат имени: <base>_YYYYMMDD_HHMMSS.log
	auto now = std::chrono::system_clock::now();
	auto time = std::chrono::system_clock::to_time_t(now);
	std::tm tm_buf{};
	localtime_s(&tm_buf, &time);

	char buf[64];
	std::snprintf(buf, sizeof(buf), "%s_%04d%02d%02d_%02d%02d%02d.log",
		config_.base_filename.c_str(),
		tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
		tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);

	return (fs::path(config_.log_directory) / buf).string();
}

void GlogCustomFormatter::OpenLogFile()
{
	current_path_ = BuildLogFilePath();
	log_file_ = std::fopen(current_path_.c_str(), "a");
	current_size_ = 0;
	if (log_file_)
	{
		// Получени реального размера файла, если файл существовал
		std::fseek(log_file_, 0, SEEK_END);
		current_size_ = static_cast<std::size_t>(std::ftell(log_file_));
	}
}

void GlogCustomFormatter::CloseLogFile()
{
	if (log_file_)
	{
		std::fclose(log_file_);
		log_file_ = nullptr;
		current_size_ = 0;
	}
}

void GlogCustomFormatter::RotateIfNeeded()
{
	if (config_.max_file_size_mb == 0) return;
	std::size_t max_bytes = config_.max_file_size_mb * 1024ULL * 1024ULL;
	if (current_size_ >= max_bytes)
	{
		CloseLogFile();
		OpenLogFile();
		EnforceMaxCount();
	}
}

std::vector<std::string> GlogCustomFormatter::CollectLogFiles() const
{
	std::vector<std::string> files;
	if (!fs::exists(config_.log_directory)) return files;

	std::string prefix = config_.base_filename + "_";
	for (const auto& entry : fs::directory_iterator(config_.log_directory))
	{
		if (!entry.is_regular_file()) continue;
		auto name = entry.path().filename().string();
		if (name.rfind(prefix, 0) == 0 &&
			name.size() > 4 &&
			name.substr(name.size() - 4) == ".log")
		{
			files.push_back(entry.path().string());
		}
	}
	// Сортировка по имени (лексикографически = хронологически)
	std::sort(files.begin(), files.end());
	return files;
}

void GlogCustomFormatter::EnforceMaxCount()
{
	if (config_.max_file_count == 0) return;
	auto files = CollectLogFiles();
	while (files.size() > config_.max_file_count)
	{
		fs::remove(files.front());
		files.erase(files.begin());
	}
}

void GlogCustomFormatter::EnforceMaxAge()
{
	if (config_.max_file_age_hours == 0) return;
	auto now = fs::file_time_type::clock::now();
	auto max_age = std::chrono::hours(config_.max_file_age_hours);

	for (const auto& path : CollectLogFiles())
	{
		auto last_write = fs::last_write_time(path);
		if ((now - last_write) > max_age)
		{
			fs::remove(path);
		}
	}
}

// фоновый поток для ротации
void GlogCustomFormatter::RotationThreadFunc()
{
	while (!stop_rotation_.load())
	{
		// Бодрствование каждые 30 секунд для проверки ротации
		for (int i = 0; i < 30 && !stop_rotation_.load(); ++i) {
			std::this_thread::sleep_for(std::chrono::seconds(1));
		}
		if (stop_rotation_.load()) break;

		std::lock_guard<std::mutex> lock(mutex_);
		if (!config_.file_enabled) continue;

		// Проверка ротации по возрасту и количеству
		EnforceMaxAge();
		EnforceMaxCount();

		// Ротация по времени (daily)
		if (config_.rotate_daily && log_file_)
		{
			auto now = std::chrono::system_clock::now();
			auto time = std::chrono::system_clock::to_time_t(now);
			std::tm tm_now{};
			localtime_s(&tm_now, &time);

			// Извлекаем дату из текущего имени файла
			// Формат: base_YYYYMMDD_HHMMSS.log
			auto fname = fs::path(current_path_).filename().string();
			std::string prefix = config_.base_filename + "_";
			if (fname.size() > prefix.size() + 8)
			{
				std::string date_part = fname.substr(prefix.size(), 8);
				char today[9];
				std::snprintf(today, sizeof(today), "%04d%02d%02d",
					tm_now.tm_year + 1900, tm_now.tm_mon + 1,
					tm_now.tm_mday);
				if (date_part != std::string(today))
				{
					CloseLogFile();
					OpenLogFile();
				}
			}
		}
	}
}