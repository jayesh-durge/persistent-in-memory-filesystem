#include <pisf/filesystem.hpp>

#include <algorithm>

namespace pisf {

Filesystem::Filesystem() {
    Node root;
    root.id = ROOT_NODE_ID;
    root.name = "/";
    root.type = NodeType::Directory;
    root.parent_id = std::nullopt;

    nodes_[ROOT_NODE_ID] = std::move(root);
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

    std::lock_guard<std::mutex> lock(mutex_);

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

    node.content = content;
    return "OK";
}

bool Filesystem::read(const std::string& path, std::string& out_content, std::string& err_msg) const {
    std::vector<std::string> components;
    if (!parse_path(path, components, err_msg)) {
        return false;
    }

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

    out_content = node.content;
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
