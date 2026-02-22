#include "log_window.h"

#include <imgui.h>
#include <filesystem>
#include <chrono>
#include <ctime>
#include <cstring>

namespace mapedit {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static const char* SeverityLabel(google::LogSeverity sev) {
    switch (sev) {
        case google::GLOG_INFO:    return "INFO";
        case google::GLOG_WARNING: return "WARN";
        case google::GLOG_ERROR:   return "ERROR";
        case google::GLOG_FATAL:   return "FATAL";
        default:                   return "???";
    }
}

static ImVec4 SeverityColor(google::LogSeverity sev) {
    switch (sev) {
        case google::GLOG_INFO:    return ImVec4(0.4f, 0.9f, 0.4f, 1.0f); // green
        case google::GLOG_WARNING: return ImVec4(1.0f, 0.9f, 0.3f, 1.0f); // yellow
        case google::GLOG_ERROR:   // fall through
        case google::GLOG_FATAL:   return ImVec4(1.0f, 0.3f, 0.3f, 1.0f); // red
        default:                   return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    }
}

// ---------------------------------------------------------------------------
// Initialize / Shutdown
// ---------------------------------------------------------------------------

void LogWindow::Initialize(const char* logDir) {
    if (m_initialized) return;

    // Create log directory.
    std::error_code ec;
    std::filesystem::create_directories(logDir, ec);

    // Build log file name: map_editor_YYYYMMDD_HHMMSS.log
    auto now = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
    localtime_s(&local, &tt);

    char filename[256];
    snprintf(filename, sizeof(filename),
             "%s/map_editor_%04d%02d%02d_%02d%02d%02d.log",
             logDir,
             local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
             local.tm_hour, local.tm_min, local.tm_sec);

    fopen_s(&m_logFile, filename, "w");

    // Reserve circular buffer to full capacity.
    m_entries.resize(kMaxLines);

    // Register as glog sink.
    google::AddLogSink(this);

    m_initialized = true;
}

void LogWindow::Shutdown() {
    if (!m_initialized) return;

    google::RemoveLogSink(this);

    if (m_logFile) {
        fclose(m_logFile);
        m_logFile = nullptr;
    }

    m_initialized = false;
}

// ---------------------------------------------------------------------------
// glog LogSink
// ---------------------------------------------------------------------------

void LogWindow::send(google::LogSeverity severity,
                     const char* /*full_filename*/,
                     const char* base_filename,
                     int line,
                     const google::LogMessageTime& logmsgtime,
                     const char* message,
                     size_t message_len) {
    // Format: "[HH:MM:SS.mmm] [LEVEL] file:line message"
    int hh = logmsgtime.hour();
    // glog wraps min() in parens to dodge Windows min macro — do the same.
    int mm = (logmsgtime.min)();
    int ss = logmsgtime.sec();
    int ms = static_cast<int>(logmsgtime.usec() / 1000);

    char prefix[128];
    snprintf(prefix, sizeof(prefix),
             "[%02d:%02d:%02d.%03d] [%-5s] %s:%d ",
             hh, mm, ss, ms,
             SeverityLabel(severity),
             base_filename ? base_filename : "?",
             line);

    std::string formatted = prefix;
    formatted.append(message, message_len);

    std::lock_guard<std::mutex> lock(m_mutex);

    // Write to file.
    if (m_logFile) {
        fprintf(m_logFile, "%s\n", formatted.c_str());
        fflush(m_logFile);
    }

    // Store in circular buffer.
    size_t idx = m_writeIndex % kMaxLines;
    m_entries[idx].severity = severity;
    m_entries[idx].text = std::move(formatted);
    ++m_writeIndex;
    ++m_totalCount;
}

// ---------------------------------------------------------------------------
// ImGui Render
// ---------------------------------------------------------------------------

void LogWindow::Render() {
    if (!m_open) return;

    // Dock to bottom-left, above the status bar (28px)
    ImGuiViewport* vp = ImGui::GetMainViewport();
    float logH = 300.0f;
    float logW = 700.0f;
    float statusBarH = 28.0f;
    ImGui::SetNextWindowPos(
        ImVec2(vp->WorkPos.x + 10.0f,
               vp->WorkPos.y + vp->WorkSize.y - statusBarH - 10.0f),
        ImGuiCond_Always, ImVec2(0.0f, 1.0f));
    ImGui::SetNextWindowSize(ImVec2(logW, logH), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.75f);
    if (!ImGui::Begin("Log", &m_open,
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoNav)) {
        ImGui::End();
        return;
    }

    // --- Toolbar row ---
    ImGui::Checkbox("INFO", &m_showInfo);
    ImGui::SameLine();
    ImGui::Checkbox("WARN", &m_showWarning);
    ImGui::SameLine();
    ImGui::Checkbox("ERROR", &m_showError);
    ImGui::SameLine();
    ImGui::Separator();
    ImGui::SameLine();

    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputTextWithHint("##filter", "Filter...", m_filterBuf, sizeof(m_filterBuf));
    ImGui::SameLine();

    if (ImGui::Button("Clear")) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_writeIndex = 0;
        m_totalCount = 0;
        for (auto& e : m_entries) e.text.clear();
    }
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &m_autoScroll);

    // Entry count.
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        size_t displayed = (m_totalCount < kMaxLines) ? m_totalCount : kMaxLines;
        ImGui::SameLine();
        ImGui::Text("(%zu entries)", displayed);
    }

    ImGui::Separator();

    // --- Scrollable log region ---
    ImGui::BeginChild("LogScrollRegion", ImVec2(0, 0), false,
                      ImGuiWindowFlags_HorizontalScrollbar);

    // Snapshot the buffer state under lock, then render outside lock.
    // For simplicity (entries are strings, cheap to iterate while locked),
    // we hold the lock for the whole render pass of the child region.
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        size_t count = (m_totalCount < kMaxLines) ? m_totalCount : kMaxLines;
        size_t startIdx = 0;
        if (m_totalCount > kMaxLines) {
            // Oldest entry is at m_writeIndex % kMaxLines.
            startIdx = m_writeIndex % kMaxLines;
        }

        bool hasFilter = (m_filterBuf[0] != '\0');

        for (size_t i = 0; i < count; ++i) {
            size_t idx = (startIdx + i) % kMaxLines;
            const LogEntry& entry = m_entries[idx];

            if (entry.text.empty()) continue;

            // Severity filter.
            if (entry.severity == google::GLOG_INFO    && !m_showInfo)    continue;
            if (entry.severity == google::GLOG_WARNING && !m_showWarning) continue;
            if ((entry.severity == google::GLOG_ERROR ||
                 entry.severity == google::GLOG_FATAL) && !m_showError)   continue;

            // Text filter (case-insensitive substring).
            if (hasFilter) {
                // Simple case-insensitive find.
                bool found = false;
                const char* haystack = entry.text.c_str();
                size_t filterLen = strlen(m_filterBuf);
                size_t textLen = entry.text.size();
                if (filterLen <= textLen) {
                    for (size_t j = 0; j <= textLen - filterLen; ++j) {
                        if (_strnicmp(haystack + j, m_filterBuf, filterLen) == 0) {
                            found = true;
                            break;
                        }
                    }
                }
                if (!found) continue;
            }

            ImGui::PushStyleColor(ImGuiCol_Text, SeverityColor(entry.severity));
            ImGui::TextUnformatted(entry.text.c_str(), entry.text.c_str() + entry.text.size());
            ImGui::PopStyleColor();
        }
    }

    if (m_autoScroll)
        ImGui::SetScrollHereY(1.0f);

    ImGui::EndChild();
    ImGui::End();
}

} // namespace mapedit
