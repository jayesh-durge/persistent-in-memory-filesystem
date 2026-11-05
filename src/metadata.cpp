#include <pisf/metadata.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

namespace pisf {

namespace {

std::atomic<uint64_t> g_tmp_counter{0};
std::atomic<bool> g_simulate_dir_sync_failure{false};

inline void write_u8(std::string& buf, uint8_t val) {
    buf.push_back(static_cast<char>(val));
}

inline void write_u32_le(std::string& buf, uint32_t val) {
    buf.push_back(static_cast<char>(val & 0xFFu));
    buf.push_back(static_cast<char>((val >> 8) & 0xFFu));
    buf.push_back(static_cast<char>((val >> 16) & 0xFFu));
    buf.push_back(static_cast<char>((val >> 24) & 0xFFu));
}

inline void write_u64_le(std::string& buf, uint64_t val) {
    for (int i = 0; i < 8; ++i) {
        buf.push_back(static_cast<char>((val >> (static_cast<unsigned>(i) * 8u)) & 0xFFu));
    }
}

inline bool read_u8(const char*& ptr, const char* end, uint8_t& val) {
    if (ptr >= end) {
        return false;
    }
    val = static_cast<uint8_t>(*ptr++);
    return true;
}

inline bool read_u32_le(const char*& ptr, const char* end, uint32_t& val) {
    if (end - ptr < 4) {
        return false;
    }
    val = static_cast<uint32_t>(static_cast<uint8_t>(ptr[0])) |
          (static_cast<uint32_t>(static_cast<uint8_t>(ptr[1])) << 8) |
          (static_cast<uint32_t>(static_cast<uint8_t>(ptr[2])) << 16) |
          (static_cast<uint32_t>(static_cast<uint8_t>(ptr[3])) << 24);
    ptr += 4;
    return true;
}

inline bool read_u64_le(const char*& ptr, const char* end, uint64_t& val) {
    if (end - ptr < 8) {
        return false;
    }
    val = 0;
    for (int i = 0; i < 8; ++i) {
        val |= (static_cast<uint64_t>(static_cast<uint8_t>(ptr[i])) << (static_cast<unsigned>(i) * 8u));
    }
    ptr += 8;
    return true;
}

} // namespace

uint32_t MetadataSerializer::compute_crc32(const uint8_t* data, size_t length) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < length; ++i) {
        crc ^= static_cast<uint32_t>(data[i]);
        for (int j = 0; j < 8; ++j) {
            if ((crc & 1u) != 0u) {
                crc = (crc >> 1) ^ 0xEDB88320u;
            } else {
                crc = (crc >> 1);
            }
        }
    }
    return ~crc;
}

bool MetadataSerializer::serialize(const MetadataState& state, std::string& out_bytes, std::string& err_msg) {
    // Validate state first to ensure we never serialize invalid metadata
    MetadataState validated_copy = state;
    if (!validate(validated_copy, err_msg)) {
        return false;
    }

    out_bytes.clear();

    // 1. Write Header:
    // Magic (4 bytes): "PISF"
    write_u32_le(out_bytes, MAGIC_HEADER);
    // Version (4 bytes): 1
    write_u32_le(out_bytes, CURRENT_VERSION);
    // Flags (4 bytes): 0
    write_u32_le(out_bytes, 0u);
    // Node Count (8 bytes)
    write_u64_le(out_bytes, static_cast<uint64_t>(state.nodes.size()));
    // Next NodeId (8 bytes)
    write_u64_le(out_bytes, state.next_node_id);
    // Active Blob Generation (8 bytes in Version 2)
    write_u64_le(out_bytes, state.blob_generation);

    // 2. Sort nodes by NodeId for deterministic binary serialization
    std::vector<NodeId> sorted_ids;
    sorted_ids.reserve(state.nodes.size());
    for (const auto& [id, _] : state.nodes) {
        sorted_ids.push_back(id);
    }
    std::sort(sorted_ids.begin(), sorted_ids.end());

    // 3. Serialize each node record
    for (NodeId id : sorted_ids) {
        const Node& node = state.nodes.at(id);

        // id (8 bytes)
        write_u64_le(out_bytes, node.id);

        // parent_id (8 bytes): 0 for root
        uint64_t pid = node.parent_id.has_value() ? *node.parent_id : 0u;
        write_u64_le(out_bytes, pid);

        // type (1 byte): 1 = Directory, 2 = File
        uint8_t type_val = (node.type == NodeType::Directory) ? 1u : 2u;
        write_u8(out_bytes, type_val);

        // name length (4 bytes) + name bytes
        if (node.name.size() > MAX_NAME_LENGTH) {
            err_msg = "ERR invalid metadata: node name exceeds max allowed size";
            return false;
        }
        write_u32_le(out_bytes, static_cast<uint32_t>(node.name.size()));
        out_bytes.append(node.name);

        // content offset and length (8 bytes each)
        write_u64_le(out_bytes, node.content_offset);
        write_u64_le(out_bytes, node.content_length);
    }

    // 4. Compute CRC32 of all bytes up to this point and append trailer
    uint32_t crc = compute_crc32(reinterpret_cast<const uint8_t*>(out_bytes.data()), out_bytes.size());
    write_u32_le(out_bytes, crc);

    return true;
}

bool MetadataSerializer::deserialize(const std::string& bytes, MetadataState& out_state, std::string& err_msg) {
    constexpr size_t HEADER_SIZE = 4 + 4 + 4 + 8 + 8; // 28 bytes
    constexpr size_t TRAILER_SIZE = 4; // 4 bytes CRC
    constexpr size_t MIN_SIZE = HEADER_SIZE + TRAILER_SIZE; // 32 bytes

    if (bytes.size() < MIN_SIZE) {
        err_msg = "ERR invalid metadata: file size too small (truncated)";
        return false;
    }

    // 1. Verify CRC32 trailer before parsing contents
    size_t data_len = bytes.size() - TRAILER_SIZE;
    uint32_t expected_crc = compute_crc32(reinterpret_cast<const uint8_t*>(bytes.data()), data_len);

    const char* trailer_ptr = bytes.data() + data_len;
    const char* trailer_end = bytes.data() + bytes.size();
    uint32_t stored_crc = 0;
    if (!read_u32_le(trailer_ptr, trailer_end, stored_crc)) {
        err_msg = "ERR invalid metadata: failed to read checksum trailer";
        return false;
    }

    if (stored_crc != expected_crc) {
        err_msg = "ERR invalid metadata: checksum mismatch (corrupted metadata)";
        return false;
    }

    const char* ptr = bytes.data();
    const char* end = bytes.data() + data_len;

    // 2. Parse Header
    uint32_t magic = 0;
    if (!read_u32_le(ptr, end, magic) || magic != MAGIC_HEADER) {
        err_msg = "ERR invalid metadata: invalid magic header";
        return false;
    }

    uint32_t version = 0;
    if (!read_u32_le(ptr, end, version)) {
        err_msg = "ERR invalid metadata: failed to read format version";
        return false;
    }
    if (version != 1 && version != CURRENT_VERSION) {
        err_msg = "ERR unsupported metadata version: " + std::to_string(version);
        return false;
    }

    uint32_t flags = 0;
    if (!read_u32_le(ptr, end, flags)) {
        err_msg = "ERR invalid metadata: failed to read flags";
        return false;
    }

    uint64_t node_count = 0;
    if (!read_u64_le(ptr, end, node_count)) {
        err_msg = "ERR invalid metadata: failed to read node count";
        return false;
    }
    if (node_count > MAX_NODE_COUNT) {
        err_msg = "ERR invalid metadata: unreasonable node count " + std::to_string(node_count);
        return false;
    }

    uint64_t next_node_id = 0;
    if (!read_u64_le(ptr, end, next_node_id)) {
        err_msg = "ERR invalid metadata: failed to read next node id";
        return false;
    }

    uint64_t blob_generation = 0;
    if (version >= 2) {
        if (!read_u64_le(ptr, end, blob_generation)) {
            err_msg = "ERR invalid metadata: failed to read blob generation";
            return false;
        }
    }

    MetadataState state;
    state.next_node_id = next_node_id;
    state.blob_generation = blob_generation;

    // 3. Parse Node records
    for (uint64_t i = 0; i < node_count; ++i) {
        Node node;

        if (!read_u64_le(ptr, end, node.id)) {
            err_msg = "ERR invalid metadata: truncated node id at record " + std::to_string(i);
            return false;
        }

        uint64_t parent_id_val = 0;
        if (!read_u64_le(ptr, end, parent_id_val)) {
            err_msg = "ERR invalid metadata: truncated parent id at record " + std::to_string(i);
            return false;
        }
        if (parent_id_val != 0) {
            node.parent_id = parent_id_val;
        } else {
            node.parent_id = std::nullopt;
        }

        uint8_t type_val = 0;
        if (!read_u8(ptr, end, type_val)) {
            err_msg = "ERR invalid metadata: truncated node type at record " + std::to_string(i);
            return false;
        }
        if (type_val == 1u) {
            node.type = NodeType::Directory;
        } else if (type_val == 2u) {
            node.type = NodeType::File;
        } else {
            err_msg = "ERR invalid metadata: unknown node type " + std::to_string(type_val);
            return false;
        }

        uint32_t name_len = 0;
        if (!read_u32_le(ptr, end, name_len)) {
            err_msg = "ERR invalid metadata: truncated name length at record " + std::to_string(i);
            return false;
        }
        if (name_len > MAX_NAME_LENGTH) {
            err_msg = "ERR invalid metadata: excessive name length " + std::to_string(name_len);
            return false;
        }
        if (static_cast<size_t>(end - ptr) < name_len) {
            err_msg = "ERR invalid metadata: truncated name string at record " + std::to_string(i);
            return false;
        }
        node.name.assign(ptr, name_len);
        ptr += name_len;

        if (!read_u64_le(ptr, end, node.content_offset)) {
            err_msg = "ERR invalid metadata: truncated content offset at record " + std::to_string(i);
            return false;
        }

        if (!read_u64_le(ptr, end, node.content_length)) {
            err_msg = "ERR invalid metadata: truncated content length at record " + std::to_string(i);
            return false;
        }

        if (state.nodes.find(node.id) != state.nodes.end()) {
            err_msg = "ERR invalid metadata: duplicate node id " + std::to_string(node.id);
            return false;
        }

        state.nodes[node.id] = std::move(node);
    }

    // 4. Ensure all record bytes were consumed exactly
    if (ptr != end) {
        err_msg = "ERR invalid metadata: unexpected trailing data in records";
        return false;
    }

    // 5. Perform structural and hierarchy validation
    if (!validate(state, err_msg)) {
        return false;
    }

    out_state = std::move(state);
    return true;
}

bool MetadataSerializer::validate(MetadataState& state, std::string& err_msg, std::optional<uint64_t> max_blob_size) {
    // 1. Root node checks
    auto root_it = state.nodes.find(ROOT_NODE_ID);
    if (root_it == state.nodes.end()) {
        err_msg = "ERR invalid metadata: root node (id 1) is missing";
        return false;
    }

    const Node& root = root_it->second;
    if (root.type != NodeType::Directory) {
        err_msg = "ERR invalid metadata: root node must be a directory";
        return false;
    }
    if (root.parent_id.has_value()) {
        err_msg = "ERR invalid metadata: root node cannot have a parent";
        return false;
    }
    if (root.name != "/") {
        err_msg = "ERR invalid metadata: root node name must be '/'";
        return false;
    }
    if (root.content_offset != 0 || root.content_length != 0) {
        err_msg = "ERR invalid metadata: root directory cannot have content extents";
        return false;
    }

    // 2. Clear existing child maps before rebuilding
    for (auto& [_, node] : state.nodes) {
        node.children.clear();
    }

    // 3. Track max NodeId to validate next_node_id
    NodeId max_id = 0;

    // 4. Validate each node's properties and register in parent's children map
    for (const auto& [id, node] : state.nodes) {
        if (id > max_id) {
            max_id = id;
        }

        if (id == ROOT_NODE_ID) {
            continue;
        }

        if (!node.parent_id.has_value() || *node.parent_id == 0) {
            err_msg = "ERR invalid metadata: non-root node " + std::to_string(id) + " has no parent";
            return false;
        }

        if (*node.parent_id == id) {
            err_msg = "ERR invalid metadata: node " + std::to_string(id) + " cannot be its own parent";
            return false;
        }

        auto parent_it = state.nodes.find(*node.parent_id);
        if (parent_it == state.nodes.end()) {
            err_msg = "ERR invalid metadata: parent " + std::to_string(*node.parent_id) + " for node " + std::to_string(id) + " missing";
            return false;
        }

        if (parent_it->second.type != NodeType::Directory) {
            err_msg = "ERR invalid metadata: parent " + std::to_string(*node.parent_id) + " is a file, not a directory";
            return false;
        }

        // Validate node name syntax
        if (node.name.empty()) {
            err_msg = "ERR invalid metadata: node " + std::to_string(id) + " has empty name";
            return false;
        }
        if (node.name == "." || node.name == "..") {
            err_msg = "ERR invalid metadata: node " + std::to_string(id) + " has reserved name '" + node.name + "'";
            return false;
        }
        if (node.name.find('/') != std::string::npos) {
            err_msg = "ERR invalid metadata: node name '" + node.name + "' contains '/'";
            return false;
        }
        for (char c : node.name) {
            auto uc = static_cast<unsigned char>(c);
            if (uc <= 32 || uc == 127) {
                err_msg = "ERR invalid metadata: node name contains whitespace or control characters";
                return false;
            }
        }

        // Duplicate child name check within directory
        auto [_, inserted] = parent_it->second.children.emplace(node.name, node.id);
        if (!inserted) {
            err_msg = "ERR invalid metadata: duplicate child name '" + node.name + "' in directory " + std::to_string(parent_it->first);
            return false;
        }

        // Content extent rules
        if (node.type == NodeType::Directory) {
            if (node.content_offset != 0 || node.content_length != 0) {
                err_msg = "ERR invalid metadata: directory " + std::to_string(id) + " has non-zero content extent";
                return false;
            }
        } else {
            // File node
            if (node.content_offset > UINT64_MAX - node.content_length) {
                err_msg = "ERR invalid metadata: file content extent overflows uint64_t limit";
                return false;
            }
            if (max_blob_size.has_value()) {
                if (node.content_offset + node.content_length > *max_blob_size) {
                    err_msg = "ERR invalid metadata: file content extent [" +
                              std::to_string(node.content_offset) + ", " +
                              std::to_string(node.content_offset + node.content_length) +
                              ") exceeds blob size " + std::to_string(*max_blob_size);
                    return false;
                }
            }
        }
    }

    // 5. Validate next_node_id
    if (state.next_node_id <= max_id) {
        err_msg = "ERR invalid metadata: next_node_id (" + std::to_string(state.next_node_id) +
                  ") must be strictly greater than maximum node id (" + std::to_string(max_id) + ")";
        return false;
    }
    if (state.next_node_id < ROOT_NODE_ID + 1) {
        err_msg = "ERR invalid metadata: next_node_id (" + std::to_string(state.next_node_id) +
                  ") cannot be less than initial threshold";
        return false;
    }

    // 6. Hierarchy tree connectivity and cycle detection
    // Each node must reach root in <= state.nodes.size() steps
    for (const auto& [id, _] : state.nodes) {
        NodeId curr = id;
        size_t steps = 0;
        while (curr != ROOT_NODE_ID) {
            if (++steps > state.nodes.size()) {
                err_msg = "ERR invalid metadata: cycle detected involving node " + std::to_string(curr);
                return false;
            }
            auto it = state.nodes.find(curr);
            if (it == state.nodes.end() || !it->second.parent_id.has_value()) {
                err_msg = "ERR invalid metadata: disconnected node " + std::to_string(curr);
                return false;
            }
            curr = *it->second.parent_id;
        }
    }

    return true;
}

void MetadataSerializer::set_simulate_dir_sync_failure(bool simulate) {
    g_simulate_dir_sync_failure.store(simulate, std::memory_order_relaxed);
}

bool MetadataSerializer::save_to_file_atomic(const std::string& data_dir, const MetadataState& state, std::string& err_msg, bool* out_committed) {
    if (out_committed != nullptr) {
        *out_committed = false;
    }

    if (data_dir.empty()) {
        err_msg = "Storage directory cannot be empty";
        return false;
    }

    std::string payload;
    if (!serialize(state, payload, err_msg)) {
        return false;
    }

    std::filesystem::path dir_path(data_dir);
    std::string tmp_name = "metadata.pisf.tmp." + std::to_string(getpid()) + "." +
                           std::to_string(g_tmp_counter.fetch_add(1));
    std::filesystem::path tmp_path = dir_path / tmp_name;
    std::filesystem::path final_path = dir_path / "metadata.pisf";

    int fd = ::open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        err_msg = "Failed to create temporary metadata file '" + tmp_path.string() + "': " + std::strerror(errno);
        return false;
    }

    const char* ptr = payload.data();
    size_t remaining = payload.size();
    bool write_ok = true;

    while (remaining > 0) {
        ssize_t n = ::write(fd, ptr, remaining);
        if (n > 0) {
            ptr += n;
            remaining -= static_cast<size_t>(n);
        } else if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            err_msg = "write() failed on temporary metadata file: " + std::string(std::strerror(errno));
            write_ok = false;
            break;
        } else {
            err_msg = "write() returned 0 unexpectedly on temporary metadata file";
            write_ok = false;
            break;
        }
    }

    if (write_ok) {
        while (true) {
            if (::fsync(fd) == 0) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            err_msg = "fsync() failed on temporary metadata file: " + std::string(std::strerror(errno));
            write_ok = false;
            break;
        }
    }

    if (::close(fd) != 0 && write_ok) {
        err_msg = "close() failed on temporary metadata file: " + std::string(std::strerror(errno));
        write_ok = false;
    }

    if (!write_ok) {
        ::unlink(tmp_path.c_str());
        return false;
    }

    // Atomically replace metadata.pisf
    if (::rename(tmp_path.c_str(), final_path.c_str()) != 0) {
        err_msg = "rename() failed: " + std::string(std::strerror(errno));
        ::unlink(tmp_path.c_str());
        return false;
    }

    // Logical commit point reached: new metadata file pathname is now visible on disk
    if (out_committed != nullptr) {
        *out_committed = true;
    }

    // Check for simulated directory sync failure
    if (g_simulate_dir_sync_failure.load(std::memory_order_relaxed)) {
        err_msg = "simulated directory sync failure after rename (metadata committed to disk)";
        return false;
    }

    // Open directory and fsync to ensure directory entry durability on disk
    int dir_fd = ::open(data_dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir_fd < 0) {
        err_msg = "open() failed on directory '" + data_dir + "': " + std::string(std::strerror(errno)) +
                  " (metadata committed on disk, but directory durability unconfirmed)";
        return false;
    }

    bool dir_sync_ok = true;
    while (true) {
        if (::fsync(dir_fd) == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        err_msg = "fsync() failed on directory '" + data_dir + "': " + std::string(std::strerror(errno)) +
                  " (metadata committed on disk, but directory durability unconfirmed)";
        dir_sync_ok = false;
        break;
    }

    if (::close(dir_fd) != 0 && dir_sync_ok) {
        err_msg = "close() failed on directory descriptor: " + std::string(std::strerror(errno)) +
                  " (metadata committed on disk, but directory durability unconfirmed)";
        dir_sync_ok = false;
    }

    if (!dir_sync_ok) {
        return false;
    }

    return true;
}

bool MetadataSerializer::load_from_file(const std::string& data_dir, MetadataState& out_state, std::string& err_msg, std::optional<uint64_t> max_blob_size) {
    if (data_dir.empty()) {
        err_msg = "Storage directory cannot be empty";
        return false;
    }

    std::filesystem::path meta_path = std::filesystem::path(data_dir) / "metadata.pisf";
    int fd = ::open(meta_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        err_msg = "Failed to open metadata file '" + meta_path.string() + "': " + std::strerror(errno);
        return false;
    }

    struct stat st{};
    if (::fstat(fd, &st) != 0) {
        err_msg = "fstat() failed on metadata file: " + std::string(std::strerror(errno));
        ::close(fd);
        return false;
    }

    if (st.st_size < 0) {
        err_msg = "Invalid metadata file size: negative length";
        ::close(fd);
        return false;
    }

    size_t file_size = static_cast<size_t>(st.st_size);
    std::string buffer;
    buffer.resize(file_size);

    char* ptr = buffer.data();
    size_t remaining = file_size;
    bool read_ok = true;

    while (remaining > 0) {
        ssize_t n = ::read(fd, ptr, remaining);
        if (n > 0) {
            ptr += n;
            remaining -= static_cast<size_t>(n);
        } else if (n == 0) {
            err_msg = "Unexpected EOF reading metadata file";
            read_ok = false;
            break;
        } else {
            if (errno == EINTR) {
                continue;
            }
            err_msg = "read() failed on metadata file: " + std::string(std::strerror(errno));
            read_ok = false;
            break;
        }
    }

    ::close(fd);

    if (!read_ok) {
        return false;
    }

    if (!deserialize(buffer, out_state, err_msg)) {
        return false;
    }

    if (max_blob_size.has_value()) {
        if (!validate(out_state, err_msg, max_blob_size)) {
            return false;
        }
    }

    return true;
}

} // namespace pisf
