#pragma once
#include <cstdint>
#include <string>

namespace mpq_cache {

// Load hash file from the given directory (e.g. "Z:\Games\wow\wotlk\").
// Creates mpq_hashes.txt there on first capture.
bool Initialize(const std::string& dir);

// Save captured hashes to disk and release resources.
void Shutdown();

// Look up a cached clean SHA1 for the given MPQ filename.
// Returns pointer to 20-byte SHA1 or nullptr if not cached.
const uint8_t* LookupHash(const std::string& filename);

// Auto-capture a clean hash for future use (first-run workflow).
void CaptureHash(const std::string& filename, const uint8_t* sha1);

// Persist captured hashes to disk.
void SaveToFile();

} // namespace mpq_cache
