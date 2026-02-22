#pragma once
#include <cstdint>
#include <cstring>
#include <vector>

namespace mapedit {

// Minimal DBC file reader for WoW 3.3.5a .dbc files.
// Format: "WDBC" header + flat records + string block.
class DbcReader {
public:
    bool Load(const uint8_t* data, size_t size);
    bool Load(const std::vector<uint8_t>& data) {
        return Load(data.data(), data.size());
    }

    uint32_t GetRecordCount() const { return m_recordCount; }
    uint32_t GetFieldCount()  const { return m_fieldCount; }
    uint32_t GetRecordSize()  const { return m_recordSize; }

    uint32_t GetUInt(uint32_t record, uint32_t field) const;
    float    GetFloat(uint32_t record, uint32_t field) const;
    int32_t  GetInt(uint32_t record, uint32_t field) const;

    // Returns pointer into the string block (valid while data is alive).
    const char* GetString(uint32_t record, uint32_t field) const;

private:
    std::vector<uint8_t> m_data;
    const uint8_t* m_records = nullptr;
    const char*    m_strings = nullptr;
    uint32_t m_recordCount = 0;
    uint32_t m_fieldCount  = 0;
    uint32_t m_recordSize  = 0;
    uint32_t m_stringBlockSize = 0;
};

} // namespace mapedit
