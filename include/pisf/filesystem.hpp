#ifndef PISF_FILESYSTEM_HPP
#define PISF_FILESYSTEM_HPP

#include <pisf/storage.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pisf {

using NodeId = uint64_t;
constexpr NodeId ROOT_NODE_ID = 1;

enum class NodeType {
    Directory,
    File
};

inline const char* to_string(NodeType type) {
    switch (type) {
        case NodeType::Directory:
            return "directory";
        case NodeType::File:
            return "file";
    }
    return "unknown";
}

struct Node {
    NodeId id{0};
    std::string name;
    NodeType type{NodeType::File};
    std::optional<NodeId> parent_id{std::nullopt};
    std::unordered_map<std::string, NodeId> children;
    uint64_t content_offset{0};
    uint64_t content_length{0};
};

class Filesystem {
public:
    Filesystem();
    explicit Filesystem(std::string data_dir);
    ~Filesystem();

    // Reinitializes the persistent storage backend to a specific directory
    bool init_storage(const std::string& data_dir, std::string& err_msg);

    // Filesystem operations (thread-safe, protected by internal mutex_)
    std::string mkdir(const std::string& path);
    std::string create(const std::string& path);
    std::string ls(const std::string& path) const;
    std::string stat(const std::string& path) const;
    std::string write(const std::string& path, const std::string& content);
    std::string read(const std::string& path) const;
    bool read(const std::string& path, std::string& out_content, std::string& err_msg) const;

    // Direct metadata inspection helper (thread-safe, useful for unit tests)
    bool exists(const std::string& path) const;
    size_t total_nodes() const;
    bool get_node_extent(const std::string& path, uint64_t& out_offset, uint64_t& out_length) const;
    const BlobStorage* storage() const;
    const std::string& data_dir() const;
    bool is_initialized() const;
    const std::string& init_error() const;

    // Test helper to simulate metadata persistence failure for rollback testing
    void set_simulate_metadata_failure(bool simulate);

    // Syntactic path parsing helper (pure function, does not access filesystem state)
    static bool parse_path(const std::string& path, std::vector<std::string>& components, std::string& err_msg);

private:
    // Resolves a sequence of path components starting from root to find target NodeId.
    // MUST be called while holding mutex_.
    bool resolve_path_locked(const std::vector<std::string>& components, NodeId& out_node_id, std::string& err_msg) const;

    // Saves current metadata state atomically to data_dir_.
    // MUST be called while holding mutex_.
    bool save_metadata_locked(std::string& err_msg) const;

    // Initializes an isolated ephemeral temporary directory for unit testing
    void init_ephemeral();

    mutable std::mutex mutex_;
    NodeId next_node_id_{ROOT_NODE_ID + 1};
    std::unordered_map<NodeId, Node> nodes_;
    std::unique_ptr<BlobStorage> storage_;
    std::string data_dir_;
    std::string ephemeral_dir_;
    bool is_ephemeral_{false};
    bool is_initialized_{false};
    std::string init_error_;
    bool simulate_metadata_failure_{false};
};

} // namespace pisf

#endif // PISF_FILESYSTEM_HPP
