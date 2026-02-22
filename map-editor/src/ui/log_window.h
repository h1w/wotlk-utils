#pragma once

#include <glog/logging.h>
#include <string>
#include <vector>
#include <mutex>
#include <cstdio>

namespace mapedit {

class LogWindow : public google::LogSink {
public:
    void Initialize(const char* logDir = "./logs");
    void Shutdown();

    // ImGui rendering
    void Render();

    bool IsOpen() const { return m_open; }
    void SetOpen(bool open) { m_open = open; }

    // glog LogSink override
    void send(google::LogSeverity severity,
              const char* full_filename,
              const char* base_filename,
              int line,
              const google::LogMessageTime& logmsgtime,
              const char* message,
              size_t message_len) override;

private:
    static constexpr size_t kMaxLines = 2000;

    struct LogEntry {
        google::LogSeverity severity;
        std::string text; // pre-formatted: "[HH:MM:SS.mmm] [LEVEL] file:line message"
    };

    std::mutex m_mutex;
    std::vector<LogEntry> m_entries; // circular buffer via index
    size_t m_writeIndex = 0;
    size_t m_totalCount = 0;
    bool m_autoScroll = true;
    bool m_showInfo = true;
    bool m_showWarning = true;
    bool m_showError = true;
    char m_filterBuf[128] = {};
    bool m_open = true;

    FILE* m_logFile = nullptr;
    bool m_initialized = false;
};

} // namespace mapedit
