#include <pisf/metadata.hpp>

#include <cassert>
#include <climits>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/stat.h>

namespace {

pisf::MetadataState create_valid_state() {
    pisf::MetadataState state;
    state.next_node_id = 4;

    pisf::Node root;
    root.id = pisf::ROOT_NODE_ID;
    root.name = "/";
    root.type = pisf::NodeType::Directory;
    root.parent_id = std::nullopt;
    state.nodes[root.id] = root;

    pisf::Node dir;
    dir.id = 2;
    dir.name = "docs";
    dir.type = pisf::NodeType::Directory;
    dir.parent_id = 1;
    state.nodes[dir.id] = dir;

    pisf::Node file;
    file.id = 3;
    file.name = "readme.txt";
    file.type = pisf::NodeType::File;
    file.parent_id = 2;
    file.content_offset = 100;
    file.content_length = 42;
    state.nodes[file.id] = file;

    return state;
}

void test_round_trip_serialization() {
    std::cout << "Running test_round_trip_serialization... ";
    pisf::MetadataState orig = create_valid_state();

    std::string bytes;
    std::string err;
    assert(pisf::MetadataSerializer::serialize(orig, bytes, err));
    assert(err.empty());
    assert(!bytes.empty());

    pisf::MetadataState restored;
    assert(pisf::MetadataSerializer::deserialize(bytes, restored, err));
    assert(err.empty());

    assert(restored.next_node_id == orig.next_node_id);
    assert(restored.nodes.size() == 3);

    // Root node
    const auto& root = restored.nodes.at(1);
    assert(root.id == 1);
    assert(root.name == "/");
    assert(root.type == pisf::NodeType::Directory);
    assert(!root.parent_id.has_value());
    assert(root.children.size() == 1);
    assert(root.children.at("docs") == 2);

    // Dir node
    const auto& dir = restored.nodes.at(2);
    assert(dir.id == 2);
    assert(dir.name == "docs");
    assert(dir.type == pisf::NodeType::Directory);
    assert(dir.parent_id == 1);
    assert(dir.children.size() == 1);
    assert(dir.children.at("readme.txt") == 3);

    // File node
    const auto& file = restored.nodes.at(3);
    assert(file.id == 3);
    assert(file.name == "readme.txt");
    assert(file.type == pisf::NodeType::File);
    assert(file.parent_id == 2);
    assert(file.children.empty());
    assert(file.content_offset == 100);
    assert(file.content_length == 42);

    std::cout << "PASSED\n";
}

void test_corrupted_checksum() {
    std::cout << "Running test_corrupted_checksum... ";
    pisf::MetadataState orig = create_valid_state();
    std::string bytes;
    std::string err;
    assert(pisf::MetadataSerializer::serialize(orig, bytes, err));

    // Flip a byte in the middle
    bytes[15] = static_cast<char>(bytes[15] ^ static_cast<char>(0x5A));

    pisf::MetadataState restored;
    assert(!pisf::MetadataSerializer::deserialize(bytes, restored, err));
    assert(err.find("checksum mismatch") != std::string::npos);

    std::cout << "PASSED\n";
}

void test_truncated_data() {
    std::cout << "Running test_truncated_data... ";
    pisf::MetadataState orig = create_valid_state();
    std::string bytes;
    std::string err;
    assert(pisf::MetadataSerializer::serialize(orig, bytes, err));

    // Truncate to very short
    std::string short_bytes = bytes.substr(0, 10);
    pisf::MetadataState restored;
    assert(!pisf::MetadataSerializer::deserialize(short_bytes, restored, err));
    assert(err.find("truncated") != std::string::npos || err.find("too small") != std::string::npos);

    // Truncate mid-records
    std::string partial_bytes = bytes.substr(0, bytes.size() - 10);
    assert(!pisf::MetadataSerializer::deserialize(partial_bytes, restored, err));

    std::cout << "PASSED\n";
}

void test_invalid_magic_and_version() {
    std::cout << "Running test_invalid_magic_and_version... ";
    pisf::MetadataState orig = create_valid_state();
    std::string bytes;
    std::string err;
    assert(pisf::MetadataSerializer::serialize(orig, bytes, err));

    // Corrupt magic header
    std::string bad_magic = bytes;
    bad_magic[0] = 'X';
    // Recalculate CRC for this bad magic test so magic check itself fails
    uint32_t new_crc = pisf::MetadataSerializer::compute_crc32(
        reinterpret_cast<const uint8_t*>(bad_magic.data()), bad_magic.size() - 4);
    bad_magic[bad_magic.size() - 4] = static_cast<char>(new_crc & 0xFFu);
    bad_magic[bad_magic.size() - 3] = static_cast<char>((new_crc >> 8) & 0xFFu);
    bad_magic[bad_magic.size() - 2] = static_cast<char>((new_crc >> 16) & 0xFFu);
    bad_magic[bad_magic.size() - 1] = static_cast<char>((new_crc >> 24) & 0xFFu);

    pisf::MetadataState restored;
    assert(!pisf::MetadataSerializer::deserialize(bad_magic, restored, err));
    assert(err.find("magic") != std::string::npos);

    // Unsupported version (e.g. version 2)
    std::string bad_ver = bytes;
    bad_ver[4] = 2; // version is at offset 4
    new_crc = pisf::MetadataSerializer::compute_crc32(
        reinterpret_cast<const uint8_t*>(bad_ver.data()), bad_ver.size() - 4);
    bad_ver[bad_ver.size() - 4] = static_cast<char>(new_crc & 0xFFu);
    bad_ver[bad_ver.size() - 3] = static_cast<char>((new_crc >> 8) & 0xFFu);
    bad_ver[bad_ver.size() - 2] = static_cast<char>((new_crc >> 16) & 0xFFu);
    bad_ver[bad_ver.size() - 1] = static_cast<char>((new_crc >> 24) & 0xFFu);

    assert(!pisf::MetadataSerializer::deserialize(bad_ver, restored, err));
    assert(err.find("unsupported metadata version") != std::string::npos);

    std::cout << "PASSED\n";
}

void test_missing_root_and_invalid_root() {
    std::cout << "Running test_missing_root_and_invalid_root... ";
    std::string err;

    // Missing root
    pisf::MetadataState no_root;
    no_root.next_node_id = 2;
    assert(!pisf::MetadataSerializer::validate(no_root, err));
    assert(err.find("root node") != std::string::npos);

    // Root is file
    pisf::MetadataState root_as_file;
    pisf::Node bad_root;
    bad_root.id = 1;
    bad_root.name = "/";
    bad_root.type = pisf::NodeType::File;
    root_as_file.nodes[1] = bad_root;
    root_as_file.next_node_id = 2;
    assert(!pisf::MetadataSerializer::validate(root_as_file, err));
    assert(err.find("directory") != std::string::npos);

    // Root has a parent
    pisf::MetadataState root_with_parent = create_valid_state();
    root_with_parent.nodes[1].parent_id = 2;
    assert(!pisf::MetadataSerializer::validate(root_with_parent, err));
    assert(err.find("cannot have a parent") != std::string::npos);

    std::cout << "PASSED\n";
}

void test_duplicate_child_names() {
    std::cout << "Running test_duplicate_child_names... ";
    pisf::MetadataState state = create_valid_state();
    std::string err;

    // Add another file with same name "readme.txt" under parent 2
    pisf::Node dup_file;
    dup_file.id = 4;
    dup_file.name = "readme.txt";
    dup_file.type = pisf::NodeType::File;
    dup_file.parent_id = 2;
    state.nodes[4] = dup_file;
    state.next_node_id = 5;

    assert(!pisf::MetadataSerializer::validate(state, err));
    assert(err.find("duplicate child name") != std::string::npos);

    std::cout << "PASSED\n";
}

void test_missing_and_file_parents() {
    std::cout << "Running test_missing_and_file_parents... ";
    std::string err;

    // Missing parent
    pisf::MetadataState missing_parent = create_valid_state();
    missing_parent.nodes[3].parent_id = 999;
    assert(!pisf::MetadataSerializer::validate(missing_parent, err));
    assert(err.find("missing") != std::string::npos);

    // Parent is a file
    pisf::MetadataState file_as_parent = create_valid_state();
    pisf::Node child_of_file;
    child_of_file.id = 4;
    child_of_file.name = "sub";
    child_of_file.type = pisf::NodeType::File;
    child_of_file.parent_id = 3; // node 3 is a file!
    file_as_parent.nodes[4] = child_of_file;
    file_as_parent.next_node_id = 5;

    assert(!pisf::MetadataSerializer::validate(file_as_parent, err));
    assert(err.find("not a directory") != std::string::npos);

    std::cout << "PASSED\n";
}

void test_directory_content_extents() {
    std::cout << "Running test_directory_content_extents... ";
    std::string err;

    pisf::MetadataState bad_dir = create_valid_state();
    bad_dir.nodes[2].content_offset = 10;
    assert(!pisf::MetadataSerializer::validate(bad_dir, err));
    assert(err.find("content extent") != std::string::npos);

    std::cout << "PASSED\n";
}

void test_content_extent_overflow() {
    std::cout << "Running test_content_extent_overflow... ";
    std::string err;

    pisf::MetadataState overflow_state = create_valid_state();
    overflow_state.nodes[3].content_offset = UINT64_MAX - 5;
    overflow_state.nodes[3].content_length = 10;

    assert(!pisf::MetadataSerializer::validate(overflow_state, err));
    assert(err.find("overflow") != std::string::npos);

    std::cout << "PASSED\n";
}

void test_blob_size_bounds_checking() {
    std::cout << "Running test_blob_size_bounds_checking... ";
    std::string err;

    pisf::MetadataState state = create_valid_state();
    // node 3 extent: offset 100, length 42 -> 142 total
    assert(pisf::MetadataSerializer::validate(state, err, 142));
    assert(pisf::MetadataSerializer::validate(state, err, 200));

    // Exceeds blob size
    assert(!pisf::MetadataSerializer::validate(state, err, 141));
    assert(err.find("exceeds blob size") != std::string::npos);

    std::cout << "PASSED\n";
}

void test_cycle_and_disconnected_node_detection() {
    std::cout << "Running test_cycle_and_disconnected_node_detection... ";
    std::string err;

    pisf::MetadataState cyclic_state = create_valid_state();
    // Create a cycle: 2 -> 4 -> 5 -> 2
    pisf::Node n4;
    n4.id = 4;
    n4.name = "n4";
    n4.type = pisf::NodeType::Directory;
    n4.parent_id = 5;

    pisf::Node n5;
    n5.id = 5;
    n5.name = "n5";
    n5.type = pisf::NodeType::Directory;
    n5.parent_id = 4;

    cyclic_state.nodes[4] = n4;
    cyclic_state.nodes[5] = n5;
    cyclic_state.next_node_id = 6;

    assert(!pisf::MetadataSerializer::validate(cyclic_state, err));
    assert(err.find("cycle") != std::string::npos || err.find("disconnected") != std::string::npos);

    std::cout << "PASSED\n";
}

void test_next_node_id_validation() {
    std::cout << "Running test_next_node_id_validation... ";
    std::string err;

    pisf::MetadataState state = create_valid_state();
    // max id is 3, next_node_id must be >= 4
    state.next_node_id = 3;
    assert(!pisf::MetadataSerializer::validate(state, err));
    assert(err.find("next_node_id") != std::string::npos);

    state.next_node_id = 2;
    assert(!pisf::MetadataSerializer::validate(state, err));

    state.next_node_id = 4;
    assert(pisf::MetadataSerializer::validate(state, err));

    std::cout << "PASSED\n";
}

void test_invalid_names() {
    std::cout << "Running test_invalid_names... ";
    std::string err;

    pisf::MetadataState state = create_valid_state();
    state.nodes[3].name = "bad/name";
    assert(!pisf::MetadataSerializer::validate(state, err));
    assert(err.find("contains '/'") != std::string::npos);

    state.nodes[3].name = "bad name";
    assert(!pisf::MetadataSerializer::validate(state, err));
    assert(err.find("whitespace") != std::string::npos);

    state.nodes[3].name = ".";
    assert(!pisf::MetadataSerializer::validate(state, err));
    assert(err.find("reserved name") != std::string::npos);

    state.nodes[3].name = "";
    assert(!pisf::MetadataSerializer::validate(state, err));
    assert(err.find("empty") != std::string::npos);

    std::cout << "PASSED\n";
}

void test_safe_replacement_preserves_previous_file_on_failure() {
    std::cout << "Running test_safe_replacement_preserves_previous_file_on_failure... ";
    std::string path_template = "/tmp/pisf-meta-saferep-XXXXXX";
    std::string temp_dir = ::mkdtemp(path_template.data());
    assert(!temp_dir.empty());

    pisf::MetadataState state1 = create_valid_state();
    std::string err;
    assert(pisf::MetadataSerializer::save_to_file_atomic(temp_dir, state1, err));

    std::string meta_path = temp_dir + "/metadata.pisf";
    std::ifstream in1(meta_path, std::ios::binary);
    std::string original_content((std::istreambuf_iterator<char>(in1)), std::istreambuf_iterator<char>());
    assert(!original_content.empty());

    // Try to save an invalid state (fails serialization/validation)
    pisf::MetadataState invalid_state = state1;
    invalid_state.next_node_id = 1; // invalid!
    assert(!pisf::MetadataSerializer::save_to_file_atomic(temp_dir, invalid_state, err));

    // Verify metadata.pisf is completely intact and unchanged
    std::ifstream in2(meta_path, std::ios::binary);
    std::string after_content((std::istreambuf_iterator<char>(in2)), std::istreambuf_iterator<char>());
    assert(after_content == original_content);

    // Verify no temporary files were left behind
    for (const auto& entry : std::filesystem::directory_iterator(temp_dir)) {
        assert(entry.path().filename().string().find(".tmp.") == std::string::npos);
    }

    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::cout << "PASSED\n";
}

void test_safe_replacement_atomic_update() {
    std::cout << "Running test_safe_replacement_atomic_update... ";
    std::string path_template = "/tmp/pisf-meta-atomic-XXXXXX";
    std::string temp_dir = ::mkdtemp(path_template.data());
    assert(!temp_dir.empty());

    pisf::MetadataState state1 = create_valid_state();
    std::string err;
    assert(pisf::MetadataSerializer::save_to_file_atomic(temp_dir, state1, err));

    // Create state2 by adding another node
    pisf::MetadataState state2 = state1;
    pisf::Node n4;
    n4.id = 4;
    n4.name = "second.txt";
    n4.type = pisf::NodeType::File;
    n4.parent_id = 2;
    state2.nodes[4] = n4;
    state2.next_node_id = 5;

    assert(pisf::MetadataSerializer::save_to_file_atomic(temp_dir, state2, err));

    pisf::MetadataState loaded;
    assert(pisf::MetadataSerializer::load_from_file(temp_dir, loaded, err));
    assert(loaded.nodes.size() == 4);
    assert(loaded.next_node_id == 5);
    assert(loaded.nodes.count(4) == 1);

    // Verify no temporary files were left behind
    for (const auto& entry : std::filesystem::directory_iterator(temp_dir)) {
        assert(entry.path().filename().string().find(".tmp.") == std::string::npos);
    }

    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::cout << "PASSED\n";
}

void test_safe_replacement_unwritable_directory() {
    std::cout << "Running test_safe_replacement_unwritable_directory... ";
    std::string path_template = "/tmp/pisf-meta-ro-XXXXXX";
    std::string temp_dir = ::mkdtemp(path_template.data());
    assert(!temp_dir.empty());

    // Save initial state while directory is writable
    pisf::MetadataState state1 = create_valid_state();
    std::string err;
    assert(pisf::MetadataSerializer::save_to_file_atomic(temp_dir, state1, err));

    std::string meta_path = temp_dir + "/metadata.pisf";
    std::ifstream in1(meta_path, std::ios::binary);
    std::string original_content((std::istreambuf_iterator<char>(in1)), std::istreambuf_iterator<char>());

    // Make directory read-only
    assert(::chmod(temp_dir.c_str(), 0555) == 0);

    pisf::MetadataState state2 = state1;
    state2.next_node_id = 10;
    // Attempt save should fail because tmp file cannot be created
    assert(!pisf::MetadataSerializer::save_to_file_atomic(temp_dir, state2, err));

    // Restore permissions
    assert(::chmod(temp_dir.c_str(), 0755) == 0);

    // Verify previous metadata.pisf is still intact
    std::ifstream in2(meta_path, std::ios::binary);
    std::string after_content((std::istreambuf_iterator<char>(in2)), std::istreambuf_iterator<char>());
    assert(after_content == original_content);

    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::cout << "PASSED\n";
}

} // namespace

int main() {
    std::cout << "=== PISF Metadata Serialization Unit Tests ===\n";

    test_round_trip_serialization();
    test_corrupted_checksum();
    test_truncated_data();
    test_invalid_magic_and_version();
    test_missing_root_and_invalid_root();
    test_duplicate_child_names();
    test_missing_and_file_parents();
    test_directory_content_extents();
    test_content_extent_overflow();
    test_blob_size_bounds_checking();
    test_cycle_and_disconnected_node_detection();
    test_next_node_id_validation();
    test_invalid_names();
    test_safe_replacement_preserves_previous_file_on_failure();
    test_safe_replacement_atomic_update();
    test_safe_replacement_unwritable_directory();

    std::cout << "All metadata serialization tests passed successfully!\n";
    return 0;
}
