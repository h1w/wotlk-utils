#include "glog_custom_formatter.hpp"
#include "dx11_app.hpp"

void setupLogger(const char* program_name)
{
	// конфигруация логгера
	GlogCustomFormatter::Config cfg;

	// уроверь логирования
	cfg.min_log_level = google::GLOG_INFO;
	cfg.verbose_level = 2;  // VLOG(1), VLOG(2) будут работать

	// консоль
	cfg.console_enabled = true;
	cfg.console_use_color = true;

	// плейсхолдеры
	//   %L  — уровень полное слово  (INFO / WARN / ERROR / FATAL)
	//   %l  — уровень один символ   (I / W / E / F)
	//   %T  — время YYYY-MM-DD HH:MM:SS.uuuuuu
	//   %F  — имя исходного файла
	//   %N  — номер строки
	//   %P  — PID
	//   %I  — Thread ID
	//   %M  — тело сообщения
	cfg.console_pattern = "[%L] %T [%F:%N] %M";

	// файл
	cfg.file_enabled = true;
	cfg.log_directory = "./logs";
	cfg.base_filename = "wotlk_utils";
	// В файле формат в стиле glog: << Lmmdd hh:mm:ss.uuuuuu pid tid file:line] msg >>
	cfg.file_pattern = "%l%T %P %I %F:%N] %M";

	// ротация
	cfg.max_file_size_mb = 50;   // ротация при 50 МБ
	cfg.max_file_count = 5;   // хранить до 5 файлов
	cfg.max_file_age_hours = 168;  // удалять файлы старше 7 дней
	cfg.rotate_daily = true; // новый файл каждый день

	// инициализация
	GlogCustomFormatter& formatter = GlogCustomFormatter::Instance();
	formatter.Initialize(cfg, program_name);
}

int main(int argc, char** argv)
{
	setupLogger(argv[0]);

	dx11_app::Initialize({.window_title = argv[0], .window_width = 1280, .window_height = 800});
	dx11_app::Run(nullptr);  // shows demo window
	dx11_app::Shutdown();

	return 0;
}