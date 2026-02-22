#include "dbc_reader.h"

namespace mapedit {

bool DbcReader::Load(const uint8_t* data, size_t size) {
    if (size < 20) return false;

    // Check magic "WDBC"
    if (data[0] != 'W' || data[1] != 'D' || data[2] != 'B' || data[3] != 'C')
        return false;

    // Keep our own copy so pointers stay valid
    m_data.assign(data, data + size);

    std::memcpy(&m_recordCount,    &m_data[4],  4);
    std::memcpy(&m_fieldCount,     &m_data[8],  4);
    std::memcpy(&m_recordSize,     &m_data[12], 4);
    std::memcpy(&m_stringBlockSize,&m_data[16], 4);

    size_t headerSize = 20;
    size_t recordsSize = static_cast<size_t>(m_recordCount) * m_recordSize;
    if (headerSize + recordsSize + m_stringBlockSize > size)
        return false;

    m_records = m_data.data() + headerSize;
    m_strings = reinterpret_cast<const char*>(m_data.data() + headerSize + recordsSize);
    return true;
}

uint32_t DbcReader::GetUInt(uint32_t record, uint32_t field) const {
    if (record >= m_recordCount || field >= m_fieldCount) return 0;
    uint32_t val = 0;
    std::memcpy(&val, m_records + record * m_recordSize + field * 4, 4);
    return val;
}

float DbcReader::GetFloat(uint32_t record, uint32_t field) const {
    if (record >= m_recordCount || field >= m_fieldCount) return 0.0f;
    float val = 0;
    std::memcpy(&val, m_records + record * m_recordSize + field * 4, 4);
    return val;
}

int32_t DbcReader::GetInt(uint32_t record, uint32_t field) const {
    if (record >= m_recordCount || field >= m_fieldCount) return 0;
    int32_t val = 0;
    std::memcpy(&val, m_records + record * m_recordSize + field * 4, 4);
    return val;
}

const char* DbcReader::GetString(uint32_t record, uint32_t field) const {
    uint32_t offset = GetUInt(record, field);
    if (offset >= m_stringBlockSize) return "";
    return m_strings + offset;
}

} // namespace mapedit
