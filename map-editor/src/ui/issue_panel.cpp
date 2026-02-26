#include "issue_panel.h"

#include <nlohmann/json.hpp>
#include <glog/logging.h>
#include <imgui.h>
#include <fstream>

using json = nlohmann::json;

namespace mapedit {

void IssuePanel::SetWorldIssues(const std::vector<GraphIssue>& issues, int totalAllMaps) {
    m_worldIssues = issues;
    m_worldTotalAllMaps = totalAllMaps;
}

void IssuePanel::SetRoadIssues(const std::vector<GraphIssue>& issues, int totalAllMaps) {
    m_roadIssues = issues;
    m_roadTotalAllMaps = totalAllMaps;
}

void IssuePanel::SetMapInfo(const char* mapName, uint32_t mapId) {
    m_mapName = mapName ? mapName : "None";
    m_mapId = mapId;
}

const std::vector<GraphIssue>& IssuePanel::CurrentIssues() const {
    return (m_graphMode == IssueGraphMode::Road) ? m_roadIssues : m_worldIssues;
}

int IssuePanel::CurrentTotal() const {
    return (m_graphMode == IssueGraphMode::Road) ? m_roadTotalAllMaps : m_worldTotalAllMaps;
}

bool IssuePanel::IsVisible(const GraphIssue& issue) const {
    bool isDismissed = m_dismissed.count(issue.key) > 0;

    // View mode filter
    if (m_viewMode == IssueViewMode::Active && isDismissed)
        return false;
    if (m_viewMode == IssueViewMode::Dismissed && !isDismissed)
        return false;

    // Gap distance filter (applies in both modes)
    if (issue.type == GraphIssue::Disconnected && m_showDisconnected) {
        if (issue.distance > m_gapMaxDistance)
            return false;
    }

    // Type filters (only in Active mode — Dismissed shows all types)
    if (m_viewMode == IssueViewMode::Dismissed)
        return true;

    switch (issue.type) {
    case GraphIssue::Disconnected:   return m_showDisconnected;
    case GraphIssue::DeadEnd:        return m_showDeadEnds;
    case GraphIssue::Orphan:         return m_showOrphans;
    case GraphIssue::DuplicateEdge:  return m_showDuplicates;
    case GraphIssue::ZeroLengthEdge: return m_showZeroLength;
    }
    return true;
}

int IssuePanel::GetActiveIssueCount() const {
    int count = 0;
    for (const auto& issue : CurrentIssues()) {
        if (!m_dismissed.count(issue.key))
            ++count;
    }
    return count;
}

int IssuePanel::GetErrorCount() const {
    int count = 0;
    for (const auto& issue : CurrentIssues()) {
        if (!m_dismissed.count(issue.key) && issue.severity == GraphIssue::Error)
            ++count;
    }
    return count;
}

int IssuePanel::GetWarningCount() const {
    int count = 0;
    for (const auto& issue : CurrentIssues()) {
        if (!m_dismissed.count(issue.key) && issue.severity == GraphIssue::Warning)
            ++count;
    }
    return count;
}

void IssuePanel::RenderIndicator() {
    int active = GetActiveIssueCount();
    int errors = GetErrorCount();

    // Position indicator at right side of status bar
    float indicatorWidth = ImGui::CalcTextSize(active > 0 ? "999 issues" : "Graph OK").x + 16.0f;
    ImGui::SameLine(ImGui::GetWindowWidth() - indicatorWidth - 8.0f);

    if (active == 0) {
        ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1.0f), "Graph OK");
    } else if (errors > 0) {
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%d issues", active);
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "%d issues", active);
    }

    if (ImGui::IsItemClicked())
        m_popupOpen = !m_popupOpen;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Click to show graph issues");
}

// Helper: render a toggle button with colored text when active, dim when inactive
static bool TypeToggleButton(const char* label, bool* active, ImVec4 activeColor, int count) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%s (%d)", label, count);

    if (*active) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(activeColor.x * 0.3f, activeColor.y * 0.3f, activeColor.z * 0.3f, 0.6f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(activeColor.x * 0.4f, activeColor.y * 0.4f, activeColor.z * 0.4f, 0.8f));
        ImGui::PushStyleColor(ImGuiCol_Text, activeColor);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.2f, 0.2f, 0.4f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.3f, 0.3f, 0.5f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 0.7f));
    }

    bool clicked = ImGui::SmallButton(buf);
    ImGui::PopStyleColor(3);

    if (clicked)
        *active = !*active;
    return clicked;
}

// Helper: render a graph mode selector button (radio-style)
static bool GraphModeButton(const char* label, bool selected) {
    if (selected) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.35f, 0.55f, 0.9f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.40f, 0.60f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.15f, 0.15f, 0.5f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.25f, 0.25f, 0.7f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 0.9f));
    }

    bool clicked = ImGui::SmallButton(label);
    ImGui::PopStyleColor(3);
    return clicked;
}

IssuePanel::Action IssuePanel::RenderPopup() {
    Action action;
    if (!m_popupOpen)
        return action;

    // Anchor at bottom-right, opens upward
    ImGuiViewport* vp = ImGui::GetMainViewport();
    float barHeight = 28.0f;
    float defaultWidth = 690.0f;
    float minWidth = 460.0f;
    float maxWidth = vp->WorkSize.x * 0.8f;
    float maxPopupHeight = 400.0f;

    ImGui::SetNextWindowPos(
        ImVec2(vp->WorkPos.x + vp->WorkSize.x - 8.0f,
               vp->WorkPos.y + vp->WorkSize.y - barHeight - 4.0f),
        ImGuiCond_Always, ImVec2(1.0f, 1.0f));
    ImGui::SetNextWindowSize(ImVec2(defaultWidth, 0.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(minWidth, 60.0f), ImVec2(maxWidth, maxPopupHeight));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoFocusOnAppearing;

    if (!ImGui::Begin("Graph Issues", &m_popupOpen, flags)) {
        ImGui::End();
        return action;
    }

    // === Graph mode selector ===
    if (GraphModeButton("World Graph", m_graphMode == IssueGraphMode::World))
        m_graphMode = IssueGraphMode::World;
    ImGui::SameLine();
    if (GraphModeButton("Road Graph", m_graphMode == IssueGraphMode::Road))
        m_graphMode = IssueGraphMode::Road;

    // === Map info + counts ===
    {
        const auto& issues = CurrentIssues();
        int mapCount = static_cast<int>(issues.size());
        int totalCount = CurrentTotal();

        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        ImGui::TextDisabled("Map: %s (%u)", m_mapName.c_str(), m_mapId);
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        ImGui::Text("%d on map", mapCount);
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        ImGui::Text("%d total", totalCount);
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        ImGui::Checkbox("Show overlay", &m_showIssueOverlay);
    }

    ImGui::Separator();

    // === Type filter buttons ===
    {
        const auto& issues = CurrentIssues();
        int cntDisc = 0, cntDead = 0, cntOrph = 0, cntDup = 0, cntZero = 0;
        for (const auto& issue : issues) {
            if (m_dismissed.count(issue.key)) continue;
            switch (issue.type) {
            case GraphIssue::Disconnected:   ++cntDisc; break;
            case GraphIssue::DeadEnd:        ++cntDead; break;
            case GraphIssue::Orphan:         ++cntOrph; break;
            case GraphIssue::DuplicateEdge:  ++cntDup;  break;
            case GraphIssue::ZeroLengthEdge: ++cntZero; break;
            }
        }

        TypeToggleButton("Gaps",       &m_showDisconnected, ImVec4(1.0f, 0.3f, 0.3f, 1.0f), cntDisc);
        ImGui::SameLine();
        TypeToggleButton("Dead-ends",  &m_showDeadEnds,     ImVec4(0.5f, 0.7f, 1.0f, 1.0f), cntDead);
        ImGui::SameLine();
        TypeToggleButton("Orphans",    &m_showOrphans,      ImVec4(1.0f, 0.8f, 0.2f, 1.0f), cntOrph);
        ImGui::SameLine();
        TypeToggleButton("Duplicates", &m_showDuplicates,   ImVec4(1.0f, 0.6f, 0.2f, 1.0f), cntDup);
        ImGui::SameLine();
        TypeToggleButton("Zero-len",   &m_showZeroLength,   ImVec4(0.8f, 0.4f, 0.8f, 1.0f), cntZero);
    }

    // === Gap distance slider ===
    if (m_showDisconnected) {
        ImGui::Text("Gaps max distance:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 4.0f);
        ImGui::SliderFloat("##gapMaxDist", &m_gapMaxDistance, 1.0f, 500.0f, "%.0f yd");
    }

    // === View mode tabs ===
    const auto& issues = CurrentIssues();
    int activeCount = GetActiveIssueCount();
    int dismissedCount = static_cast<int>(issues.size()) - activeCount;

    {
        char buf[64];
        snprintf(buf, sizeof(buf), "Active (%d)", activeCount);
        if (GraphModeButton(buf, m_viewMode == IssueViewMode::Active))
            m_viewMode = IssueViewMode::Active;
        ImGui::SameLine();
        snprintf(buf, sizeof(buf), "Dismissed (%d)", dismissedCount);
        if (GraphModeButton(buf, m_viewMode == IssueViewMode::Dismissed))
            m_viewMode = IssueViewMode::Dismissed;
    }

    ImGui::Separator();

    // Issue list
    int visibleCount = 0;
    for (const auto& issue : issues) {
        if (!IsVisible(issue))
            continue;

        ++visibleCount;
        bool isDismissed = m_dismissed.count(issue.key) > 0;

        ImGui::PushID(issue.key.c_str());

        // Severity indicator
        ImVec4 sevColor;
        const char* sevIcon;
        switch (issue.severity) {
        case GraphIssue::Error:   sevColor = ImVec4(1.0f, 0.3f, 0.3f, 1.0f); sevIcon = "[!]"; break;
        case GraphIssue::Warning: sevColor = ImVec4(1.0f, 0.8f, 0.2f, 1.0f); sevIcon = "[?]"; break;
        default:                  sevColor = ImVec4(0.5f, 0.7f, 1.0f, 1.0f); sevIcon = "[i]"; break;
        }

        if (isDismissed)
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f);

        ImGui::TextColored(sevColor, "%s", sevIcon);
        ImGui::SameLine();
        ImGui::TextUnformatted(issue.message.c_str());
        ImGui::SameLine();

        // Action buttons
        if (ImGui::SmallButton("Go To")) {
            action.type = Action::GoTo;
            action.worldX = issue.worldX;
            action.worldY = issue.worldY;
            action.nodeIds = issue.nodeIds;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Select")) {
            action.type = Action::Select;
            action.nodeIds = issue.nodeIds;
            action.edgeIndices = issue.edgeIndices;
        }
        ImGui::SameLine();
        if (m_viewMode == IssueViewMode::Dismissed) {
            if (ImGui::SmallButton("Restore"))
                m_dismissed.erase(issue.key);
        } else {
            if (ImGui::SmallButton("Dismiss"))
                m_dismissed.insert(issue.key);
        }

        if (isDismissed)
            ImGui::PopStyleVar();

        ImGui::PopID();
    }

    if (visibleCount == 0) {
        if (issues.empty())
            ImGui::TextDisabled("No issues found.");
        else if (m_viewMode == IssueViewMode::Dismissed)
            ImGui::TextDisabled("No dismissed issues.");
        else
            ImGui::TextDisabled("All issues filtered or dismissed.");
    }

    ImGui::End();
    return action;
}

void IssuePanel::LoadDismissed(const std::string& path) {
    m_dismissFilePath = path;
    m_dismissed.clear();

    std::ifstream file(path);
    if (!file.is_open()) return;

    try {
        json doc = json::parse(file);
        if (doc.contains("dismissed") && doc["dismissed"].is_array()) {
            for (const auto& key : doc["dismissed"])
                m_dismissed.insert(key.get<std::string>());
        }
        LOG(INFO) << "[IssuePanel] Loaded " << m_dismissed.size() << " dismissed issues";
    } catch (const json::parse_error&) {
        // Ignore corrupt file
    }
}

void IssuePanel::SaveDismissed(const std::string& path) const {
    const std::string& savePath = path.empty() ? m_dismissFilePath : path;
    if (savePath.empty()) return;
    if (m_dismissed.empty()) return;

    json doc;
    json arr = json::array();
    for (const auto& key : m_dismissed)
        arr.push_back(key);
    doc["dismissed"] = arr;

    std::ofstream file(savePath);
    if (file.is_open())
        file << doc.dump(2);
}

} // namespace mapedit
