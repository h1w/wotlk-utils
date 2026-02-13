#pragma once

#include <glog/logging.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class GlogCustomFormatter
{
public:
	// конфигруация
	struct Config
	{
		// уровень логирования
		google::LogSeverity min_log_level = google::GLOG_INFO;
		int verbose_level = 0;

		// консоль
		bool console_enabled = true;
		bool console_use_color = true;

		// printf стиль
		std::string console_pattern = "[%L] %T [%F:%N] %M";

		// файл
		bool file_enabled = true;
		std::string log_directory = "./logs";
		std::string base_filename = "wotlk_utils";
		// формат строки в файле (те же плейсхолдеры)
		std::string file_pattern = "%T %L %P %I %F:%N] %M";

		// ротация
		std::size_t max_file_size_mb = 100; // 100 МБ
		std::size_t max_file_count = 5; // 5 файлов
		std::size_t max_file_age_hours = 168; // 7 дней

		// ротация по времени
		bool rotate_daily = false; // принудительная ротация раз в сутки
	};

	// синглтон
	static GlogCustomFormatter& Instance();

	// запрет копирования и перемещения
	GlogCustomFormatter(const GlogCustomFormatter&) = delete;
	GlogCustomFormatter& operator=(const GlogCustomFormatter&) = delete;
	GlogCustomFormatter(GlogCustomFormatter&&) = delete;
	GlogCustomFormatter& operator=(GlogCustomFormatter&&) = delete;

	// жизненный цикл
	// инициализация glog и применение конфигурации
	// @param cfg структура конфигурации
	// @param argv0 имя программы
	void Initialize(const Config& cfg, const char* argv0 = "app");

	// корректное завершение работы логгера, закрытие файлов, остановка, фоновый поток ротации
	void Shutdown();

	// изменение параметров на лету
	void SetMinLogLevel(google::LogSeverity level);
	void SetVerboseLevel(int level);
	void SetConsoleEnabled(bool enabled);
	void SetFileEnabled(bool enabled);
	void SetConsolePattern(const std::string& pattern);
	void SetFilePattern(const std::string& pattern);

	// получение текущей конфигруации
	const Config& GetConfig() const { return config_; }

	// принудительная ротация
	void RotateNow();

private:
	GlogCustomFormatter();
	~GlogCustomFormatter();

	// кастомный glog-sink
	class CustomSink : public google::LogSink
	{
	public:
		explicit CustomSink(GlogCustomFormatter& owner);
		~CustomSink() override;

		void send(google::LogSeverity severity,
			const char* full_filename,
			const char* base_filename,
			int line,
			const google::LogMessageTime& logmsgtime,
			const char* message,
			size_t message_len) override;

	private:
		GlogCustomFormatter& owner_;
	};

	// форматирование
	static std::string FormatMessage(const std::string& pattern,
		google::LogSeverity severity,
		const char* filename,
		int line,
		const google::LogMessageTime& logmsgtime,
		const char* message,
		std::size_t message_len);

	static char SeverityToChar(google::LogSeverity severity);
	static const char* SeverityToString(google::LogSeverity severity);
	static std::string SeverityColorPrefix(google::LogSeverity severity);
	static std::string ColorReset();

	// файловый ввод и ротация
	void WriteToFile(const std::string& formatted, google::LogSeverity severity);
	void OpenLogFile();
	void CloseLogFile();
	void RotateIfNeeded();
	void EnforceMaxCount();
	void EnforceMaxAge();
	std::string BuildLogFilePath() const;
	std::vector<std::string> CollectLogFiles() const;

	// фоновый поток для ротации
	void RotationThreadFunc();

	// данные
	Config config_;
	std::mutex mutex_;
	FILE* log_file_ = nullptr;
	std::size_t current_size_ = 0;
	std::string current_path_;
	bool initialized_ = false;

	std::unique_ptr<CustomSink> sink_;

	std::thread rotation_thread_;
	std::atomic<bool> stop_rotation_ = false;
};
