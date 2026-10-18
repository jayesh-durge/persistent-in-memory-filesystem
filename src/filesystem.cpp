#include <pisf/filesystem.hpp>
#include <pisf/metadata.hpp>

#include <algorithm>
#include <filesystem>

#include <pisf/common.hpp>

namespace pisf {

Filesystem::Filesystem() {
    init_ephemeral();
}

Filesystem::Filesystem(std::string data_dir) {
    std::string err;
    init_storage(data_dir, err);
}

Filesystem::~Filesystem() {
    if (!ephemeral_dir_.empty()) {
        std::error_code ec;
        std::filesystem::remove_all(ephemeral_dir_, ec);
    }
}

void Filesystem::init_ephemeral() {
    is_ephemeral_ = true;
    std::string path_template = "/tmp/pisf-fs-XXXXXX";
    char* d = ::mkdtemp(path_template.data());
    if (d != nullptr) {
        ephemeral_dir_ = d;
        std::string err;
        init_storage(ephemeral_dir_, err);
        is_ephemeral_ = true;
    } else {
        // Fallback: in-memory root if mkdtemp fails
        Node root;
        root.id = ROOT_NODE_ID;
        root.name = "/";
        root.type = NodeType::Directory;
        root.parent_id = std::nullopt;
        nodes_[ROOT_NODE_ID] = std::move(root);
        next_node_id_ = ROOT_NODE_ID + 1;
        is_initialized_ = true;
    }
}

bool Filesystem::init_storage(const std::string& data_dir, std::string& err_msg) {
    std::lock_guard<std::mutex> lock(mutex_);
    is_ephemeral_ = false;

    // If we previously had an ephemeral directory, clean it up
    if (!ephemeral_dir_.empty() && ephemeral_dir_ != data_dir) {
        std::error_code ec;
        std::filesystem::remove_all(ephemeral_dir_, ec);
        ephemeral_dir_.clear();
    }

    if (data_dir.empty()) {
        err_msg = "Storage directory cannot be empty";
        is_initialized_ = false;
        init_error_ = err_msg;
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(data_dir, ec);
    if (ec) {
        err_msg = "Failed to create storage directory '" + data_dir + "': " + ec.message();
        is_initialized_ = false;
        init_error_ = err_msg;
        return false;
    }

    std::string meta_path = (std::filesystem::path(data_dir) / "metadata.pisf").string();
    bool meta_exists = std::filesystem::is_regular_file(meta_path, ec);

    if (meta_exists) {
        MetadataState state;
        if (!MetadataSerializer::load_from_file(data_dir, state, err_msg)) {
            is_initialized_ = false;
            init_error_ = err_msg;
            return false;
        }

        std::string blob_filename = BlobStorage::format_blob_filename(state.blob_generation);
        std::string active_blob_path = (std::filesystem::path(data_dir) / blob_filename).string();
        if (!std::filesystem::is_regular_file(active_blob_path, ec)) {
            err_msg = "ERR missing blob for active generation: '" + blob_filename +
                      "' is missing in '" + data_dir + "'";
            is_initialized_ = false;
            init_error_ = err_msg;
            return false;
        }

        auto new_storage = std::make_shared<BlobStorage>(data_dir, state.blob_generation);
        if (!new_storage->init(err_msg)) {
            is_initialized_ = false;
            init_error_ = err_msg;
            return false;
        }

        uint64_t blob_size = new_storage->get_size();
        if (!MetadataSerializer::validate(state, err_msg, blob_size)) {
            is_initialized_ = false;
            init_error_ = err_msg;
            return false;
        }

        data_dir_ = data_dir;
        nodes_ = std::move(state.nodes);
        next_node_id_ = state.next_node_id;
        blob_generation_ = state.blob_generation;
        storage_ = std::move(new_storage);
        is_initialized_ = true;
        init_error_.clear();
        return true;
    }

    // Metadata file does not exist
    bool any_blob_exists = false;
    for (const auto& entry : std::filesystem::directory_iterator(data_dir, ec)) {
        std::string fname = entry.path().filename().string();
        if (fname == "data.blob" ||
            (fname.rfind("data-", 0) == 0 && fname.size() > 5 && fname.substr(fname.size() - 5) == ".blob")) {
            any_blob_exists = true;
            break;
        }
    }

    if (any_blob_exists) {
        err_msg = "ERR missing metadata: blob storage file exists in '" + data_dir +
                  "' but 'metadata.pisf' is missing; refusing to initialize empty filesystem";
        is_initialized_ = false;
        init_error_ = err_msg;
        return false;
    }

    for (const auto& entry : std::filesystem::directory_iterator(data_dir, ec)) {
        std::string filename = entry.path().filename().string();
        if (!filename.empty() && filename[0] != '.') {
            err_msg = "ERR missing metadata: storage directory '" + data_dir +
                      "' is not empty but 'metadata.pisf' is missing; refusing to initialize empty filesystem";
            is_initialized_ = false;
            init_error_ = err_msg;
            return false;
        }
    }

    // Fresh directory: initialize fresh root and save initial metadata
    nodes_.clear();
    Node root;
    root.id = ROOT_NODE_ID;
    root.name = "/";
    root.type = NodeType::Directory;
    root.parent_id = std::nullopt;
    nodes_[ROOT_NODE_ID] = std::move(root);
    next_node_id_ = ROOT_NODE_ID + 1;
    blob_generation_ = 0;

    auto new_storage = std::make_shared<BlobStorage>(data_dir, 0);
    if (!new_storage->init(err_msg)) {
        is_initialized_ = false;
        init_error_ = err_msg;
        return false;
    }

    data_dir_ = data_dir;
    storage_ = std::move(new_storage);

    MetadataState initial_state;
    initial_state.next_node_id = next_node_id_;
    initial_state.blob_generation = blob_generation_;
    initial_state.nodes = nodes_;
    if (!MetadataSerializer::save_to_file_atomic(data_dir_, initial_state, err_msg)) {
        is_initialized_ = false;
        init_error_ = err_msg;
        return false;
    }

    is_initialized_ = true;
    init_error_.clear();
    return true;
}

bool Filesystem::save_metadata_locked(std::string& err_msg) const {
    bool dummy = false;
    return save_metadata_locked(err_msg, dummy);
}

bool Filesystem::save_metadata_locked(std::string& err_msg, bool& out_committed) const {
    out_committed = false;
    if (simulate_metadata_failure_) {
        err_msg = "simulated metadata persistence failure";
        return false;
    }
    if (data_dir_.empty() || is_ephemeral_) {
        out_committed = true;
        return true;
    }
    MetadataState state;
    state.next_node_id = next_node_id_;
    state.blob_generation = blob_generation_;
    state.nodes = nodes_;
    return MetadataSerializer::save_to_file_atomic(data_dir_, state, err_msg, &out_committed);
}

uint64_t Filesystem::blob_generation() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return blob_generation_;
}

void Filesystem::set_simulate_metadata_failure(bool simulate) {
    std::lock_guard<std::mutex> lock(mutex_);
    simulate_metadata_failure_ = simulate;
}

const std::string& Filesystem::data_dir() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return data_dir_;
}

bool Filesystem::is_initialized() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return is_initialized_;
}

const std::string& Filesystem::init_error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return init_error_;
}

const BlobStorage* Filesystem::storage() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return storage_.get();
}

bool Filesystem::get_node_extent(const std::string& path, uint64_t& out_offset, uint64_t& out_length) const {
    std::vector<std::string> components;
    std::string err_msg;
    if (!parse_path(path, components, err_msg)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    NodeId target_id = 0;
    if (!resolve_path_locked(components, target_id, err_msg)) {
        return false;
    }

    auto it = nodes_.find(target_id);
    if (it == nodes_.end() || it->second.type != NodeType::File) {
        return false;
    }

    out_offset = it->second.content_offset;
    out_length = it->second.content_length;
    return true;
}

bool Filesystem::parse_path(const std::string& path, std::vector<std::string>& components, std::string& err_msg) {
    components.clear();

    if (path.empty() || path[0] != '/') {
        err_msg = "ERR invalid path: must be absolute (starting with '/')";
        return false;
    }

    size_t i = 1;
    size_t n = path.size();
    while (i < n) {
        // Skip repeated separators
        while (i < n && path[i] == '/') {
            ++i;
        }
        if (i >= n) {
            break;
        }
        size_t start = i;
        while (i < n && path[i] != '/') {
            ++i;
        }
        std::string comp = path.substr(start, i - start);

        // Reject relative navigation tokens
        if (comp == "." || comp == "..") {
            err_msg = "ERR invalid path component: '.' and '..' are not supported";
            return false;
        }

        // Reject components containing whitespace or control characters
        for (char c : comp) {
            auto uc = static_cast<unsigned char>(c);
            if (uc <= 32 || uc == 127) {
                err_msg = "ERR invalid path component: whitespace and control characters not allowed";
                return false;
            }
        }

        components.push_back(std::move(comp));
    }

    return true;
}

bool Filesystem::resolve_path_locked(const std::vector<std::string>& components, NodeId& out_node_id, std::string& err_msg) const {
    NodeId curr_id = ROOT_NODE_ID;

    for (const auto& comp : components) {
        auto it = nodes_.find(curr_id);
        if (it == nodes_.end()) {
            err_msg = "ERR internal error: node " + std::to_string(curr_id) + " missing";
            return false;
        }

        const Node& curr_node = it->second;
        if (curr_node.type != NodeType::Directory) {
            err_msg = "ERR not a directory: '" + curr_node.name + "' is a file";
            return false;
        }

        auto child_it = curr_node.children.find(comp);
        if (child_it == curr_node.children.end()) {
            err_msg = "ERR not found: '" + comp + "' does not exist";
            return false;
        }

        curr_id = child_it->second;
    }

    out_node_id = curr_id;
    return true;
}

std::string Filesystem::mkdir(const std::string& path) {
    std::vector<std::string> components;
    std::string err_msg;
    if (!parse_path(path, components, err_msg)) {
        return err_msg;
    }

    if (components.empty()) {
        return "ERR already exists";
    }

    std::string target_name = components.back();
    components.pop_back();

    std::lock_guard<std::mutex> lock(mutex_);

    NodeId parent_id = 0;
    if (!resolve_path_locked(components, parent_id, err_msg)) {
        return err_msg;
    }

    auto parent_it = nodes_.find(parent_id);
    if (parent_it == nodes_.end()) {
        return "ERR internal error: parent node missing";
    }

    Node& parent_node = parent_it->second;
    if (parent_node.type != NodeType::Directory) {
        return "ERR not a directory: parent is a file";
    }

    if (parent_node.children.find(target_name) != parent_node.children.end()) {
        return "ERR already exists";
    }

    NodeId new_id = next_node_id_++;
    Node new_node;
    new_node.id = new_id;
    new_node.name = target_name;
    new_node.type = NodeType::Directory;
    new_node.parent_id = parent_id;

    nodes_[new_id] = std::move(new_node);
    parent_node.children[target_name] = new_id;

    std::string persist_err;
    bool committed = false;
    if (!save_metadata_locked(persist_err, committed)) {
        if (!committed) {
            nodes_.erase(new_id);
            parent_node.children.erase(target_name);
            next_node_id_ = new_id;
            return "ERR metadata persistence failed: " + persist_err;
        }
        return "ERR metadata directory sync failed: " + persist_err;
    }

    return "OK";
}

std::string Filesystem::create(const std::string& path) {
    std::vector<std::string> components;
    std::string err_msg;
    if (!parse_path(path, components, err_msg)) {
        return err_msg;
    }

    if (components.empty()) {
        return "ERR cannot create file at root";
    }

    std::string target_name = components.back();
    components.pop_back();

    std::lock_guard<std::mutex> lock(mutex_);

    NodeId parent_id = 0;
    if (!resolve_path_locked(components, parent_id, err_msg)) {
        return err_msg;
    }

    auto parent_it = nodes_.find(parent_id);
    if (parent_it == nodes_.end()) {
        return "ERR internal error: parent node missing";
    }

    Node& parent_node = parent_it->second;
    if (parent_node.type != NodeType::Directory) {
        return "ERR not a directory: parent is a file";
    }

    if (parent_node.children.find(target_name) != parent_node.children.end()) {
        return "ERR already exists";
    }

    NodeId new_id = next_node_id_++;
    Node new_node;
    new_node.id = new_id;
    new_node.name = target_name;
    new_node.type = NodeType::File;
    new_node.parent_id = parent_id;

    nodes_[new_id] = std::move(new_node);
    parent_node.children[target_name] = new_id;

    std::string persist_err;
    bool committed = false;
    if (!save_metadata_locked(persist_err, committed)) {
        if (!committed) {
            nodes_.erase(new_id);
            parent_node.children.erase(target_name);
            next_node_id_ = new_id;
            return "ERR metadata persistence failed: " + persist_err;
        }
        return "ERR metadata directory sync failed: " + persist_err;
    }

    return "OK";
}

std::string Filesystem::rm(const std::string& path) {
    std::vector<std::string> components;
    std::string err_msg;
    if (!parse_path(path, components, err_msg)) {
        return err_msg;
    }

    if (components.empty()) {
        return "ERR cannot remove root";
    }

    std::lock_guard<std::mutex> lock(mutex_);

    NodeId target_id = 0;
    if (!resolve_path_locked(components, target_id, err_msg)) {
        return err_msg;
    }

    auto it = nodes_.find(target_id);
    if (it == nodes_.end()) {
        return "ERR internal error: node missing";
    }

    if (it->second.type != NodeType::File) {
        return "ERR not a file: use RMDIR to remove directories";
    }

    // Locate and record the parent node before mutation
    NodeId parent_id = it->second.parent_id.value();
    auto parent_it = nodes_.find(parent_id);
    if (parent_it == nodes_.end()) {
        return "ERR internal error: parent node missing";
    }

    Node& parent_node = parent_it->second;
    const std::string target_name = it->second.name;
    Node old_node = it->second;

    // Mutate in-memory: remove child entry from parent and erase the file node
    parent_node.children.erase(target_name);
    nodes_.erase(it);

    std::string persist_err;
    bool committed = false;
    if (!save_metadata_locked(persist_err, committed)) {
        if (!committed) {
            // Rename did not happen — restore parent entry and re-insert node
            nodes_[target_id] = std::move(old_node);
            parent_node.children[target_name] = target_id;
            return "ERR metadata persistence failed: " + persist_err;
        }
        // Rename succeeded — deletion is committed on disk; do NOT undo in-memory state
        return "ERR metadata directory sync failed: " + persist_err;
    }

    // Blob extents for this file become orphaned (data.blob is append-only; no compaction)
    return "OK";
}

std::string Filesystem::rmdir(const std::string& path) {
    std::vector<std::string> components;
    std::string err_msg;
    if (!parse_path(path, components, err_msg)) {
        return err_msg;
    }

    if (components.empty()) {
        return "ERR cannot remove root";
    }

    std::lock_guard<std::mutex> lock(mutex_);

    NodeId target_id = 0;
    if (!resolve_path_locked(components, target_id, err_msg)) {
        return err_msg;
    }

    auto it = nodes_.find(target_id);
    if (it == nodes_.end()) {
        return "ERR internal error: node missing";
    }

    if (it->second.type != NodeType::Directory) {
        return "ERR not a directory: use RM to remove files";
    }

    if (!it->second.children.empty()) {
        return "ERR directory not empty";
    }

    NodeId parent_id = it->second.parent_id.value();
    auto parent_it = nodes_.find(parent_id);
    if (parent_it == nodes_.end()) {
        return "ERR internal error: parent node missing";
    }

    Node& parent_node = parent_it->second;
    const std::string target_name = it->second.name;
    Node old_node = it->second;

    // Mutate in-memory
    parent_node.children.erase(target_name);
    nodes_.erase(it);

    std::string persist_err;
    bool committed = false;
    if (!save_metadata_locked(persist_err, committed)) {
        if (!committed) {
            nodes_[target_id] = std::move(old_node);
            parent_node.children[target_name] = target_id;
            return "ERR metadata persistence failed: " + persist_err;
        }
        return "ERR metadata directory sync failed: " + persist_err;
    }

    return "OK";
}

std::string Filesystem::mv(const std::string& src_path, const std::string& dst_path) {
    // dst_path is the full destination: parent directory + new name.
    // e.g.  MV /docs/old.txt /docs/new.txt   → rename within same dir
    //       MV /docs/old.txt /archive/old.txt → move to /archive, keep name

    std::vector<std::string> src_components;
    std::vector<std::string> dst_components;
    std::string err_msg;

    if (!parse_path(src_path, src_components, err_msg)) {
        return err_msg;
    }
    if (!parse_path(dst_path, dst_components, err_msg)) {
        return err_msg;
    }

    if (src_components.empty()) {
        return "ERR cannot move root";
    }
    if (dst_components.empty()) {
        return "ERR invalid destination: cannot replace root";
    }

    std::lock_guard<std::mutex> lock(mutex_);

    // --- Resolve source ---
    NodeId src_id = 0;
    if (!resolve_path_locked(src_components, src_id, err_msg)) {
        return err_msg;
    }

    auto src_it = nodes_.find(src_id);
    if (src_it == nodes_.end()) {
        return "ERR internal error: source node missing";
    }
    Node& src_node = src_it->second;

    NodeId old_parent_id = src_node.parent_id.value();
    const std::string old_name = src_node.name;

    // --- Resolve destination parent (all components except the last = new name) ---
    std::string new_name = dst_components.back();
    std::vector<std::string> dst_parent_components(dst_components.begin(),
                                                    dst_components.end() - 1);
    NodeId new_parent_id = 0;
    if (!resolve_path_locked(dst_parent_components, new_parent_id, err_msg)) {
        return err_msg;
    }

    auto new_parent_it = nodes_.find(new_parent_id);
    if (new_parent_it == nodes_.end()) {
        return "ERR internal error: destination parent node missing";
    }
    Node& new_parent_node = new_parent_it->second;
    if (new_parent_node.type != NodeType::Directory) {
        return "ERR destination parent is not a directory";
    }

    // --- Handle the no-op case: same parent and same name ---
    if (new_parent_id == old_parent_id && new_name == old_name) {
        return "OK"; // nothing to do
    }

    // --- Validate no name collision at destination ---
    if (new_parent_node.children.count(new_name) > 0) {
        return "ERR already exists: '" + new_name + "' already exists in destination directory";
    }

    // --- Guard against moving a directory into itself or a descendant ---
    if (src_node.type == NodeType::Directory) {
        // Walk up from new_parent_id; if we encounter src_id, it's a descendant
        NodeId check_id = new_parent_id;
        while (true) {
            if (check_id == src_id) {
                return "ERR invalid move: cannot move a directory into itself or a descendant";
            }
            auto check_it = nodes_.find(check_id);
            if (check_it == nodes_.end() || !check_it->second.parent_id.has_value()) {
                break; // reached root
            }
            check_id = check_it->second.parent_id.value();
        }
    }

    // --- Apply in-memory mutations ---
    auto old_parent_it = nodes_.find(old_parent_id);
    if (old_parent_it == nodes_.end()) {
        return "ERR internal error: source parent node missing";
    }
    Node& old_parent_node = old_parent_it->second;

    // 1. Remove from old parent
    old_parent_node.children.erase(old_name);
    // 2. Add to new parent
    new_parent_node.children[new_name] = src_id;
    // 3. Update node itself
    src_node.name      = new_name;
    src_node.parent_id = new_parent_id;

    // --- Persist ---
    std::string persist_err;
    bool committed = false;
    if (!save_metadata_locked(persist_err, committed)) {
        if (!committed) {
            // Full rollback
            src_node.name      = old_name;
            src_node.parent_id = old_parent_id;
            new_parent_node.children.erase(new_name);
            old_parent_node.children[old_name] = src_id;
            return "ERR metadata persistence failed: " + persist_err;
        }
        // Rename succeeded — do NOT undo
        return "ERR metadata directory sync failed: " + persist_err;
    }

    return "OK";
}

std::string Filesystem::ls(const std::string& path) const {
    std::vector<std::string> components;
    std::string err_msg;
    if (!parse_path(path, components, err_msg)) {
        return err_msg;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    NodeId target_id = 0;
    if (!resolve_path_locked(components, target_id, err_msg)) {
        return err_msg;
    }

    auto it = nodes_.find(target_id);
    if (it == nodes_.end()) {
        return "ERR internal error: node missing";
    }

    const Node& node = it->second;
    if (node.type != NodeType::Directory) {
        return "ERR not a directory";
    }

    std::vector<std::string> child_names;
    child_names.reserve(node.children.size());
    for (const auto& [name, id] : node.children) {
        static_cast<void>(id);
        child_names.push_back(name);
    }

    std::sort(child_names.begin(), child_names.end());

    std::string result;
    for (size_t i = 0; i < child_names.size(); ++i) {
        if (i > 0) {
            result += ' ';
        }
        result += child_names[i];
    }

    return result;
}

std::string Filesystem::stat(const std::string& path) const {
    std::vector<std::string> components;
    std::string err_msg;
    if (!parse_path(path, components, err_msg)) {
        return err_msg;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    NodeId target_id = 0;
    if (!resolve_path_locked(components, target_id, err_msg)) {
        return err_msg;
    }

    auto it = nodes_.find(target_id);
    if (it == nodes_.end()) {
        return "ERR internal error: node missing";
    }

    const Node& node = it->second;
    return "name=" + node.name + " type=" + to_string(node.type) + " id=" + std::to_string(node.id);
}

std::string Filesystem::write(const std::string& path, const std::string& content) {
    if (content.find('\n') != std::string::npos || content.find('\r') != std::string::npos) {
        return "ERR invalid content: newline characters not allowed";
    }

    std::vector<std::string> components;
    std::string err_msg;
    if (!parse_path(path, components, err_msg)) {
        return err_msg;
    }

    // Hold Filesystem::mutex_ for the entire write operation — path validation,
    // blob append, extent update, and metadata persistence — so that concurrent
    // writes to the same (or any) path cannot interleave their blob appends and
    // metadata commits.  Reads use a snapshot pattern and are not affected.
    //
    // Lock ordering: Filesystem::mutex_ -> BlobStorage::mutex_ (append acquires
    // the latter internally).  No other code acquires these in the opposite order,
    // so there is no deadlock risk.
    std::lock_guard<std::mutex> lock(mutex_);

    // 1. Validate path and node type under lock
    NodeId target_id = 0;
    if (!resolve_path_locked(components, target_id, err_msg)) {
        return err_msg;
    }

    auto it = nodes_.find(target_id);
    if (it == nodes_.end()) {
        return "ERR internal error: node missing";
    }

    Node& node = it->second;
    if (node.type != NodeType::File) {
        return "ERR not a file";
    }

    uint64_t old_offset = node.content_offset;
    uint64_t old_length = node.content_length;

    // 2. Append content to persistent blob storage (lock still held)
    uint64_t offset = 0;
    uint64_t length = 0;
    if (!storage_ || !storage_->append(content, offset, length, err_msg)) {
        return "ERR storage write failed: " + err_msg;
    }

    // 3. Update the node's extent and persist metadata under the same lock
    node.content_offset = offset;
    node.content_length = length;

    std::string persist_err;
    bool committed = false;
    if (!save_metadata_locked(persist_err, committed)) {
        if (!committed) {
            // Rename did not occur: roll back in-memory extent
            node.content_offset = old_offset;
            node.content_length = old_length;
            return "ERR metadata persistence failed: " + persist_err;
        }
        // Rename succeeded: new metadata is on disk; do NOT roll back in-memory state
        return "ERR metadata directory sync failed: " + persist_err;
    }

    // 4. Return success only after blob content and metadata are both durable
    return "OK";
}

bool Filesystem::read(const std::string& path, std::string& out_content, std::string& err_msg) const {
    std::vector<std::string> components;
    if (!parse_path(path, components, err_msg)) {
        return false;
    }

    uint64_t offset = 0;
    uint64_t length = 0;
    std::shared_ptr<BlobStorage> active_storage;

    // 1. Hold filesystem mutex while obtaining a consistent snapshot of content location
    //    and a shared reference to the active storage generation.
    {
        std::lock_guard<std::mutex> lock(mutex_);

        NodeId target_id = 0;
        if (!resolve_path_locked(components, target_id, err_msg)) {
            return false;
        }

        auto it = nodes_.find(target_id);
        if (it == nodes_.end()) {
            err_msg = "ERR internal error: node missing";
            return false;
        }

        const Node& node = it->second;
        if (node.type != NodeType::File) {
            err_msg = "ERR not a file";
            return false;
        }

        offset = node.content_offset;
        length = node.content_length;
        active_storage = storage_;
    }

    // 2. Handle empty file consistently without disk I/O
    if (length == 0) {
        out_content.clear();
        return true;
    }

    // 3. Read content from persistent blob storage using the snapshotted storage handle
    if (!active_storage || !active_storage->read_range(offset, length, out_content, err_msg)) {
        return false;
    }

    return true;
}

std::string Filesystem::read(const std::string& path) const {
    std::string content;
    std::string err_msg;
    if (!read(path, content, err_msg)) {
        return err_msg;
    }
    return content;
}

bool Filesystem::exists(const std::string& path) const {
    std::vector<std::string> components;
    std::string err_msg;
    if (!parse_path(path, components, err_msg)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    NodeId target_id = 0;
    return resolve_path_locked(components, target_id, err_msg);
}

size_t Filesystem::total_nodes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return nodes_.size();
}

} // namespace pisf
