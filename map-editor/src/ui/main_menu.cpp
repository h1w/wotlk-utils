#include "main_menu.h"
#include "../data/world_graph_data.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <imgui.h>

namespace mapedit {

static std::string OpenFileDialog(const char* filter, const char* title) {
    char filename[MAX_PATH] = {};
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameA(&ofn))
        return filename;
    return {};
}

static std::string SaveFileDialog(const char* filter, const char* title) {
    char filename[MAX_PATH] = {};
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    if (GetSaveFileNameA(&ofn))
        return filename;
    return {};
}

MainMenu::Actions MainMenu::Render(const WorldGraphData& graph) {
    Actions actions;

    // Note: This is called inside an existing BeginMainMenuBar block from App
    if (ImGui::BeginMenu("Graph")) {
        if (ImGui::MenuItem("Open Graph JSON...")) {
            actions.graphFilePath = OpenFileDialog(
                "JSON files\0*.json\0All files\0*.*\0", "Open World Graph");
            if (!actions.graphFilePath.empty())
                actions.openGraph = true;
        }
        if (ImGui::MenuItem("Open Road Graph...")) {
            actions.roadGraphFilePath = OpenFileDialog(
                "JSON files\0*.json\0All files\0*.*\0", "Open Road Graph");
            if (!actions.roadGraphFilePath.empty())
                actions.openRoadGraph = true;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Save", "Ctrl+S", false, graph.IsLoaded())) {
            actions.saveGraph = true;
        }
        if (ImGui::MenuItem("Save As...", nullptr, false, graph.IsLoaded())) {
            actions.graphFilePath = SaveFileDialog(
                "JSON files\0*.json\0All files\0*.*\0", "Save World Graph");
            if (!actions.graphFilePath.empty())
                actions.saveGraphAs = true;
        }
        ImGui::EndMenu();
    }

    return actions;
}

} // namespace mapedit
