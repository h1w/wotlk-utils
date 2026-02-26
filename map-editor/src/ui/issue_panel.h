#pragma once

#include "../editor/graph_validator.h"
#include <string>
#include <vector>
#include <unordered_set>

namespace mapedit {

enum class IssueGraphMode { World, Road };
enum class IssueViewMode { Active, Dismissed };

class IssuePanel {
public:
    // Set issues for each graph (current map only)
    void SetWorldIssues(const std::vector<GraphIssue>& issues, int totalAllMaps);
    void SetRoadIssues(const std::vector<GraphIssue>& issues, int totalAllMaps);
    void SetMapInfo(const char* mapName, uint32_t mapId);

    // Which graph mode is currently selected in the panel
    IssueGraphMode GetGraphMode() const { return m_graphMode; }

    // Overlay controls
    float GetGapMaxDistance() const { return m_gapMaxDistance; }
    bool GetShowIssueOverlay() const { return m_showIssueOverlay; }

    // State persistence
    void SetGraphMode(IssueGraphMode mode)  { m_graphMode = mode; }
    void SetViewMode(IssueViewMode mode)    { m_viewMode = mode; }
    bool IsPopupOpen() const                { return m_popupOpen; }
    void SetPopupOpen(bool open)            { m_popupOpen = open; }
    void SetGapMaxDistance(float d)          { m_gapMaxDistance = d; }
    void SetShowIssueOverlay(bool v)        { m_showIssueOverlay = v; }
    bool GetShowDisconnected() const        { return m_showDisconnected; }
    void SetShowDisconnected(bool v)        { m_showDisconnected = v; }
    bool GetShowDeadEnds() const            { return m_showDeadEnds; }
    void SetShowDeadEnds(bool v)            { m_showDeadEnds = v; }
    bool GetShowOrphans() const             { return m_showOrphans; }
    void SetShowOrphans(bool v)             { m_showOrphans = v; }
    bool GetShowDuplicates() const          { return m_showDuplicates; }
    void SetShowDuplicates(bool v)          { m_showDuplicates = v; }
    bool GetShowZeroLength() const          { return m_showZeroLength; }
    void SetShowZeroLength(bool v)          { m_showZeroLength = v; }
    IssueViewMode GetViewMode() const       { return m_viewMode; }

    // Render the clickable indicator inside the status bar window.
    void RenderIndicator();

    struct Action {
        enum Type { None, GoTo, Select } type = None;
        float worldX = 0.0f, worldY = 0.0f;
        std::vector<uint32_t> nodeIds;
        std::vector<size_t> edgeIndices;
    };

    // Render the popup (call after status bar End). Returns action if user clicked Go To/Select.
    Action RenderPopup();

    void LoadDismissed(const std::string& path);
    void SaveDismissed(const std::string& path) const;

    // Counts for the currently selected graph mode
    int GetActiveIssueCount() const;
    int GetErrorCount() const;
    int GetWarningCount() const;

private:
    bool IsVisible(const GraphIssue& issue) const;
    const std::vector<GraphIssue>& CurrentIssues() const;
    int CurrentTotal() const;

    // Per-graph data
    std::vector<GraphIssue> m_worldIssues;
    std::vector<GraphIssue> m_roadIssues;
    int m_worldTotalAllMaps = 0;
    int m_roadTotalAllMaps = 0;

    // Map info
    std::string m_mapName;
    uint32_t m_mapId = 0;

    // UI state
    IssueGraphMode m_graphMode = IssueGraphMode::World;
    IssueViewMode m_viewMode = IssueViewMode::Active;
    std::unordered_set<std::string> m_dismissed;
    bool m_popupOpen = false;
    std::string m_dismissFilePath;
    float m_gapMaxDistance = 20.0f;
    bool  m_showIssueOverlay = true;

    // Type filter toggles
    bool m_showDisconnected = true;
    bool m_showDeadEnds     = true;
    bool m_showOrphans      = true;
    bool m_showDuplicates   = true;
    bool m_showZeroLength   = true;
};

} // namespace mapedit
