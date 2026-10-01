#include <pisf/metadata.hpp>

#include <algorithm>
#include <climits>
#include <cstdint>
#include <vector>

namespace pisf {

namespace {

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

} // namespace pisf
