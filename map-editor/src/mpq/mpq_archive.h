#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace mapedit {

// Wraps StormLib to open multiple MPQ archives from a WoW Data directory
// and read files by their internal path (e.g. "Interface\\WorldMap\\Azeroth\\Azeroth1.blp").
class MpqArchiveSet {
public:
    ~MpqArchiveSet() { Close(); }

    // Open all MPQ archives in the WoW Data directory, ordered by priority.
    bool Open(const std::string& dataDir);
    void Close();

    // Read a file from the highest-priority archive that contains it.
    std::vector<uint8_t> ReadFile(const std::string& internalPath) const;

    // Check if a file exists in any archive.
    bool HasFile(const std::string& internalPath) const;

    // List files matching a mask (e.g. "World\\Minimaps\\Azeroth\\*").
    // Returns up to maxResults file paths.
    std::vector<std::string> ListFiles(const std::string& mask, int maxResults = 20) const;

    bool IsOpen() const { return !m_archives.empty(); }
    const std::string& GetDataDir() const { return m_dataDir; }

private:
    std::string m_dataDir;
    std::vector<void*> m_archives; // HANDLE values from StormLib
};

} // namespace mapedit
