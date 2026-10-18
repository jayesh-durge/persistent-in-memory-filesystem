#ifndef PISF_METADATA_HPP
#define PISF_METADATA_HPP

#include <pisf/filesystem.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pisf {

// In-memory representation of the persistent filesystem metadata state
struct MetadataState {
    NodeId next_node_id{ROOT_NODE_ID + 1};
    uint64_t blob_generation{0};
    std::unordered_map<NodeId, Node> nodes;
};

// Responsible for robust serialization, deserialization, and structural validation
// of PISF filesystem metadata.
class MetadataSerializer {
public:
    static constexpr uint32_t MAGIC_HEADER = 0x50495346; // ASCII "PISF" (0x50, 0x49, 0x53, 0x46)
    static constexpr uint32_t CURRENT_VERSION = 2;
    static constexpr size_t MAX_NAME_LENGTH = 4096;
    static constexpr uint64_t MAX_NODE_COUNT = 1000000;

    // Serializes MetadataState into binary payload
    static bool serialize(const MetadataState& state, std::string& out_bytes, std::string& err_msg);

    // Deserializes binary payload into MetadataState, reconstructing children maps and validating all rules
    static bool deserialize(const std::string& bytes, MetadataState& out_state, std::string& err_msg);

    // Performs complete structural and semantic validation on MetadataState.
    // If max_blob_size is provided, file extents are checked against the blob size.
    static bool validate(MetadataState& state, std::string& err_msg, std::optional<uint64_t> max_blob_size = std::nullopt);

    // Atomically saves metadata to metadata.pisf inside data_dir using a temp file and rename.
    // If out_committed is non-null, it is set to true as soon as rename() succeeds.
    static bool save_to_file_atomic(const std::string& data_dir, const MetadataState& state, std::string& err_msg, bool* out_committed = nullptr);

    // Test helper to simulate directory sync failure after rename has succeeded
    static void set_simulate_dir_sync_failure(bool simulate);

    // Loads, parses, and validates metadata from metadata.pisf inside data_dir
    static bool load_from_file(const std::string& data_dir, MetadataState& out_state, std::string& err_msg, std::optional<uint64_t> max_blob_size = std::nullopt);

    // Computes standard IEEE 802.3 CRC32 checksum over buffer
    static uint32_t compute_crc32(const uint8_t* data, size_t length);
};

} // namespace pisf

#endif // PISF_METADATA_HPP
