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

} // namespace pisf
