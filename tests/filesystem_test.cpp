#include <pisf/filesystem.hpp>
#include <pisf/metadata.hpp>

#include <atomic>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

void test_root_initialization() {
    std::cout << "Running test_root_initialization... ";
    pisf::Filesystem fs;

    assert(fs.exists("/"));
    assert(fs.total_nodes() == 1);

    std::string root_stat = fs.stat("/");
    assert(root_stat == "name=/ type=directory id=1");

    std::string root_ls = fs.ls("/");
    assert(root_ls.empty());

    std::cout << "PASSED\n";
}

void test_create_and_mkdir() {
    std::cout << "Running test_create_and_mkdir... ";
    pisf::Filesystem fs;

    assert(fs.mkdir("/projects") == "OK");
    assert(fs.create("/notes.txt") == "OK");
    assert(fs.create("/projects/demo.txt") == "OK");

    assert(fs.exists("/projects"));
    assert(fs.exists("/notes.txt"));
    assert(fs.exists("/projects/demo.txt"));
    assert(fs.total_nodes() == 4);

    std::cout << "PASSED\n";
}

void test_listing_and_sorting() {
    std::cout << "Running test_listing_and_sorting... ";
    pisf::Filesystem fs;

    fs.mkdir("/z_dir");
    fs.mkdir("/a_dir");
    fs.create("/m_file.txt");

    // Root children should be sorted alphabetically: "a_dir m_file.txt z_dir"
    std::string root_listing = fs.ls("/");
    assert(root_listing == "a_dir m_file.txt z_dir");

    // Empty directory listing should be empty string
    std::string empty_listing = fs.ls("/a_dir");
    assert(empty_listing.empty());

    // Listing a file should fail
    std::string file_listing = fs.ls("/m_file.txt");
    assert(file_listing == "ERR not a directory");

    std::cout << "PASSED\n";
}

void test_stat_metadata() {
    std::cout << "Running test_stat_metadata... ";
    pisf::Filesystem fs;

    fs.mkdir("/docs");
    fs.create("/docs/guide.txt");

    std::string root_stat = fs.stat("/");
    assert(root_stat == "name=/ type=directory id=1");

    std::string dir_stat = fs.stat("/docs");
    assert(dir_stat.rfind("name=docs type=directory id=", 0) == 0);

    std::string file_stat = fs.stat("/docs/guide.txt");
    assert(file_stat.rfind("name=guide.txt type=file id=", 0) == 0);

    // Non-existent target stat
    std::string missing_stat = fs.stat("/missing.txt");
    assert(missing_stat.rfind("ERR not found", 0) == 0);

    std::cout << "PASSED\n";
}

void test_missing_and_invalid_parents() {
    std::cout << "Running test_missing_and_invalid_parents... ";
    pisf::Filesystem fs;

    // Creating child under missing parent
    assert(fs.mkdir("/nonexistent/sub") == "ERR not found: 'nonexistent' does not exist");
    assert(fs.create("/nonexistent/file.txt") == "ERR not found: 'nonexistent' does not exist");

    // Creating child under a file
    assert(fs.create("/file.txt") == "OK");
    assert(fs.mkdir("/file.txt/sub") == "ERR not a directory: parent is a file");
    assert(fs.create("/file.txt/sub.txt") == "ERR not a directory: parent is a file");

    std::cout << "PASSED\n";
}

void test_duplicate_names() {
    std::cout << "Running test_duplicate_names... ";
    pisf::Filesystem fs;

    assert(fs.mkdir("/dup_dir") == "OK");
    assert(fs.mkdir("/dup_dir") == "ERR already exists");
    assert(fs.create("/dup_dir") == "ERR already exists");

    assert(fs.create("/dup_file") == "OK");
    assert(fs.create("/dup_file") == "ERR already exists");
    assert(fs.mkdir("/dup_file") == "ERR already exists");

    std::cout << "PASSED\n";
}

void test_identical_names_in_different_dirs() {
    std::cout << "Running test_identical_names_in_different_dirs... ";
    pisf::Filesystem fs;

    assert(fs.mkdir("/d1") == "OK");
    assert(fs.mkdir("/d2") == "OK");

    assert(fs.create("/d1/readme.md") == "OK");
    assert(fs.create("/d2/readme.md") == "OK");

    assert(fs.exists("/d1/readme.md"));
    assert(fs.exists("/d2/readme.md"));

    std::string stat1 = fs.stat("/d1/readme.md");
    std::string stat2 = fs.stat("/d2/readme.md");
    assert(stat1 != stat2); // IDs must be different

    std::cout << "PASSED\n";
}

void test_path_normalization_and_validation() {
    std::cout << "Running test_path_normalization_and_validation... ";
    pisf::Filesystem fs;

    // Redundant slashes and normalization (parent must be created first)
    assert(fs.mkdir("//nested//") == "OK");
    assert(fs.mkdir("//nested///sub//") == "OK");
    assert(fs.exists("/nested/sub"));
    assert(fs.stat("//nested//sub/") == "name=sub type=directory id=3");

    // Relative paths
    std::vector<std::string> comps;
    std::string err;
    assert(!pisf::Filesystem::parse_path("relative/path", comps, err));
    assert(fs.mkdir("relative/path") == "ERR invalid path: must be absolute (starting with '/')");

    // '.' and '..'
    assert(!pisf::Filesystem::parse_path("/a/./b", comps, err));
    assert(fs.create("/a/./b") == "ERR invalid path component: '.' and '..' are not supported");
    assert(!pisf::Filesystem::parse_path("/a/../b", comps, err));
    assert(fs.mkdir("/a/../b") == "ERR invalid path component: '.' and '..' are not supported");

    // Empty path
    assert(!pisf::Filesystem::parse_path("", comps, err));

    // Whitespace in component
    assert(!pisf::Filesystem::parse_path("/a b", comps, err));

    // Creating root
    assert(fs.mkdir("/") == "ERR already exists");
    assert(fs.create("/") == "ERR cannot create file at root");

    std::cout << "PASSED\n";
}

void test_concurrent_same_path_creation() {
    std::cout << "Running test_concurrent_same_path_creation... ";
    pisf::Filesystem fs;

    constexpr int NUM_THREADS = 10;
    std::vector<std::thread> threads;
    std::atomic<int> success_count{0};
    std::atomic<int> already_exists_count{0};

    for (int i = 0; i < NUM_THREADS; ++i) {
        threads.emplace_back([&fs, &success_count, &already_exists_count]() {
            std::string res = fs.create("/contested.txt");
            if (res == "OK") {
                success_count.fetch_add(1);
            } else if (res == "ERR already exists") {
                already_exists_count.fetch_add(1);
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    assert(success_count.load() == 1);
    assert(already_exists_count.load() == NUM_THREADS - 1);
    assert(fs.exists("/contested.txt"));

    std::cout << "PASSED\n";
}

void test_concurrent_independent_operations() {
    std::cout << "Running test_concurrent_independent_operations... ";
    pisf::Filesystem fs;

    constexpr int NUM_CLIENTS = 8;
    constexpr int OPS_PER_CLIENT = 50;

    // Create a base directory for each client first
    for (int c = 0; c < NUM_CLIENTS; ++c) {
        std::string dir = "/client_" + std::to_string(c);
        assert(fs.mkdir(dir) == "OK");
    }

    std::vector<std::thread> threads;
    std::atomic<bool> failed{false};

    for (int c = 0; c < NUM_CLIENTS; ++c) {
        threads.emplace_back([&fs, c, &failed]() {
            std::string dir = "/client_" + std::to_string(c);
            for (int i = 0; i < OPS_PER_CLIENT; ++i) {
                std::string file = dir + "/file_" + std::to_string(i) + ".txt";
                if (fs.create(file) != "OK") {
                    failed.store(true);
                }
                if (fs.stat(file).rfind("name=file_", 0) == std::string::npos) {
                    failed.store(true);
                }
            }
            std::string listing = fs.ls(dir);
            if (listing.empty()) {
                failed.store(true);
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    assert(!failed.load());
    // 1 root + NUM_CLIENTS dirs + NUM_CLIENTS * OPS_PER_CLIENT files
    size_t expected_nodes = 1 + static_cast<size_t>(NUM_CLIENTS) + static_cast<size_t>(NUM_CLIENTS * OPS_PER_CLIENT);
    assert(fs.total_nodes() == expected_nodes);

    std::cout << "PASSED\n";
}

void test_read_empty_file() {
    std::cout << "Running test_read_empty_file... ";
    pisf::Filesystem fs;

    assert(fs.create("/empty.txt") == "OK");
    assert(fs.read("/empty.txt") == "");

    std::string content;
    std::string err;
    assert(fs.read("/empty.txt", content, err));
    assert(content.empty());

    std::cout << "PASSED\n";
}

void test_write_and_read_content() {
    std::cout << "Running test_write_and_read_content... ";
    pisf::Filesystem fs;

    assert(fs.create("/notes.txt") == "OK");
    assert(fs.write("/notes.txt", "Hello from PISF") == "OK");
    assert(fs.read("/notes.txt") == "Hello from PISF");

    std::cout << "PASSED\n";
}

void test_replace_content() {
    std::cout << "Running test_replace_content... ";
    pisf::Filesystem fs;

    assert(fs.create("/doc.txt") == "OK");
    assert(fs.write("/doc.txt", "Initial version") == "OK");
    assert(fs.read("/doc.txt") == "Initial version");

    assert(fs.write("/doc.txt", "Replaced with completely new contents") == "OK");
    assert(fs.read("/doc.txt") == "Replaced with completely new contents");

    std::cout << "PASSED\n";
}

void test_content_containing_spaces() {
    std::cout << "Running test_content_containing_spaces... ";
    pisf::Filesystem fs;

    assert(fs.create("/spaces.txt") == "OK");
    std::string spaced_content = "  leading,   multiple internal   spaces, and trailing   ";
    assert(fs.write("/spaces.txt", spaced_content) == "OK");
    assert(fs.read("/spaces.txt") == spaced_content);

    std::cout << "PASSED\n";
}

void test_missing_file_read_write() {
    std::cout << "Running test_missing_file_read_write... ";
    pisf::Filesystem fs;

    assert(fs.read("/missing.txt").rfind("ERR not found", 0) == 0);
    assert(fs.write("/missing.txt", "content").rfind("ERR not found", 0) == 0);
    assert(fs.read("/nonexistent/file.txt").rfind("ERR not found", 0) == 0);
    assert(fs.write("/nonexistent/file.txt", "content").rfind("ERR not found", 0) == 0);

    std::cout << "PASSED\n";
}

void test_directory_read_write() {
    std::cout << "Running test_directory_read_write... ";
    pisf::Filesystem fs;

    assert(fs.mkdir("/mydir") == "OK");

    // Root directory
    assert(fs.read("/") == "ERR not a file");
    assert(fs.write("/", "data") == "ERR not a file");

    // Subdirectory
    assert(fs.read("/mydir") == "ERR not a file");
    assert(fs.write("/mydir", "data") == "ERR not a file");

    std::cout << "PASSED\n";
}

void test_empty_content_and_newlines() {
    std::cout << "Running test_empty_content_and_newlines... ";
    pisf::Filesystem fs;

    assert(fs.create("/test.txt") == "OK");
    assert(fs.write("/test.txt", "Initial") == "OK");
    assert(fs.read("/test.txt") == "Initial");

    // Writing empty content clears the file
    assert(fs.write("/test.txt", "") == "OK");
    assert(fs.read("/test.txt") == "");

    // Embedded newlines must be rejected
    assert(fs.write("/test.txt", "line1\nline2") == "ERR invalid content: newline characters not allowed");
    assert(fs.write("/test.txt", "line1\rline2") == "ERR invalid content: newline characters not allowed");
    assert(fs.read("/test.txt") == ""); // unchanged

    std::cout << "PASSED\n";
}

void test_concurrent_different_files() {
    std::cout << "Running test_concurrent_different_files... ";
    pisf::Filesystem fs;

    constexpr int NUM_THREADS = 8;
    constexpr int ITERATIONS = 100;
    std::vector<std::thread> threads;
    std::atomic<bool> failed{false};

    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&fs, t, &failed]() {
            std::string path = "/thread_" + std::to_string(t) + ".txt";
            if (fs.create(path) != "OK") {
                failed.store(true);
                return;
            }

            for (int i = 0; i < ITERATIONS; ++i) {
                std::string payload = "data_thread_" + std::to_string(t) + "_iter_" + std::to_string(i);
                if (fs.write(path, payload) != "OK") {
                    failed.store(true);
                    return;
                }
                if (fs.read(path) != payload) {
                    failed.store(true);
                    return;
                }
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    assert(!failed.load());
    std::cout << "PASSED\n";
}

void test_concurrent_same_file_read_write() {
    std::cout << "Running test_concurrent_same_file_read_write... ";
    pisf::Filesystem fs;

    assert(fs.create("/shared.txt") == "OK");
    assert(fs.write("/shared.txt", "INITIAL_STATE_0000000000000000000000") == "OK");

    constexpr int NUM_WRITERS = 4;
    constexpr int NUM_READERS = 4;
    constexpr int ITERATIONS = 300;

    std::vector<std::string> valid_payloads;
    valid_payloads.push_back("INITIAL_STATE_0000000000000000000000");
    for (int w = 0; w < NUM_WRITERS; ++w) {
        valid_payloads.push_back("WRITER_" + std::to_string(w) + "_PAYLOAD_ABCDEFGHIJKLMN");
    }

    std::vector<std::thread> threads;
    std::atomic<bool> stop_flag{false};
    std::atomic<bool> corruption_detected{false};

    // Writers
    for (int w = 0; w < NUM_WRITERS; ++w) {
        threads.emplace_back([&fs, w, &valid_payloads]() {
            const std::string& payload = valid_payloads[static_cast<size_t>(w + 1)];
            for (int i = 0; i < ITERATIONS; ++i) {
                fs.write("/shared.txt", payload);
            }
        });
    }

    // Readers
    for (int r = 0; r < NUM_READERS; ++r) {
        threads.emplace_back([&fs, &valid_payloads, &corruption_detected, &stop_flag]() {
            while (!stop_flag.load(std::memory_order_relaxed)) {
                std::string content = fs.read("/shared.txt");
                bool valid = false;
                for (const auto& expected : valid_payloads) {
                    if (content == expected) {
                        valid = true;
                        break;
                    }
                }
                if (!valid) {
                    corruption_detected.store(true);
                    break;
                }
            }
        });
    }

    // Join writers first
    for (int i = 0; i < NUM_WRITERS; ++i) {
        threads[static_cast<size_t>(i)].join();
    }

    // Stop and join readers
    stop_flag.store(true);
    for (size_t i = static_cast<size_t>(NUM_WRITERS); i < threads.size(); ++i) {
        threads[i].join();
    }

    assert(!corruption_detected.load());
    std::cout << "PASSED\n";
}

void test_storage_backed_write_extents() {
    std::cout << "Running test_storage_backed_write_extents... ";
    std::string path_template = "/tmp/pisf-fs-extents-test-XXXXXX";
    std::string temp_dir = ::mkdtemp(path_template.data());
    assert(!temp_dir.empty());

    {
        pisf::Filesystem fs(temp_dir);
        assert(fs.create("/doc.txt") == "OK");

        std::string first_content = "Hello, persistent blob!";
        assert(fs.write("/doc.txt", first_content) == "OK");

        uint64_t off1 = 999, len1 = 999;
        assert(fs.get_node_extent("/doc.txt", off1, len1));
        assert(off1 == 0);
        assert(len1 == first_content.size());

        // Read directly from storage blob to confirm bytes were written to disk
        std::string blob_bytes;
        std::string err;
        assert(fs.storage()->read_range(off1, len1, blob_bytes, err));
        assert(blob_bytes == first_content);

        // Replace file content: should allocate new extent without overwriting old extent
        std::string second_content = "Replaced with brand new content!";
        assert(fs.write("/doc.txt", second_content) == "OK");

        uint64_t off2 = 999, len2 = 999;
        assert(fs.get_node_extent("/doc.txt", off2, len2));
        assert(off2 == len1);
        assert(len2 == second_content.size());

        // Verify second content in blob
        assert(fs.storage()->read_range(off2, len2, blob_bytes, err));
        assert(blob_bytes == second_content);

        // Verify old content is still intact in blob at off1
        assert(fs.storage()->read_range(off1, len1, blob_bytes, err));
        assert(blob_bytes == first_content);
    }

    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::cout << "PASSED\n";
}

void test_storage_backed_read_reflects_blob() {
    std::cout << "Running test_storage_backed_read_reflects_blob... ";
    std::string path_template = "/tmp/pisf-fs-readblob-test-XXXXXX";
    std::string temp_dir = ::mkdtemp(path_template.data());
    assert(!temp_dir.empty());

    {
        pisf::Filesystem fs(temp_dir);
        assert(fs.create("/target.txt") == "OK");

        std::string original = "InitialBlobData123";
        assert(fs.write("/target.txt", original) == "OK");
        assert(fs.read("/target.txt") == original);

        uint64_t offset = 0, length = 0;
        assert(fs.get_node_extent("/target.txt", offset, length));

        // Directly patch the blob file on disk
        std::string blob_file = temp_dir + "/data.blob";
        std::fstream file(blob_file, std::ios::in | std::ios::out | std::ios::binary);
        assert(file.is_open());
        file.seekp(static_cast<std::streamoff>(offset));
        std::string modified = "PatchedBlobData123";
        file.write(modified.data(), static_cast<std::streamsize>(modified.size()));
        file.flush();
        file.close();

        // Reading via filesystem should now return the modified bytes from disk,
        // proving content is not stored in RAM.
        assert(fs.read("/target.txt") == modified);
    }

    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::cout << "PASSED\n";
}

void test_filesystem_restore_from_metadata() {
    std::cout << "Running test_filesystem_restore_from_metadata... ";
    std::string path_template = "/tmp/pisf-fs-restore-test-XXXXXX";
    std::string temp_dir = ::mkdtemp(path_template.data());
    assert(!temp_dir.empty());

    // 1. Manually write data.blob
    std::string content = "PersistentDocContent999";
    std::string blob_path = temp_dir + "/data.blob";
    {
        std::ofstream out(blob_path, std::ios::binary);
        out << "padding_data_at_start"; // 21 bytes
        out << content; // 23 bytes (offset 21, length 23)
    }

    // 2. Prepare metadata state matching data.blob
    pisf::MetadataState state;
    state.next_node_id = 5;

    pisf::Node root;
    root.id = 1;
    root.name = "/";
    root.type = pisf::NodeType::Directory;
    root.parent_id = std::nullopt;
    state.nodes[1] = root;

    pisf::Node dir;
    dir.id = 2;
    dir.name = "workspace";
    dir.type = pisf::NodeType::Directory;
    dir.parent_id = 1;
    state.nodes[2] = dir;

    pisf::Node file;
    file.id = 3;
    file.name = "doc.txt";
    file.type = pisf::NodeType::File;
    file.parent_id = 2;
    file.content_offset = 21;
    file.content_length = 23;
    state.nodes[3] = file;

    std::string err;
    assert(pisf::MetadataSerializer::save_to_file_atomic(temp_dir, state, err));

    // 3. Construct Filesystem on this directory
    {
        pisf::Filesystem fs(temp_dir);
        assert(fs.is_initialized());
        assert(fs.exists("/"));
        assert(fs.exists("/workspace"));
        assert(fs.exists("/workspace/doc.txt"));

        assert(fs.ls("/") == "workspace");
        assert(fs.ls("/workspace") == "doc.txt");
        assert(fs.read("/workspace/doc.txt") == content);
        assert(fs.stat("/workspace/doc.txt") == "name=doc.txt type=file id=3");

        // Create new file and verify NodeId continues from restored next_node_id (5)
        assert(fs.create("/workspace/new.txt") == "OK");
        assert(fs.stat("/workspace/new.txt") == "name=new.txt type=file id=5");
    }

    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::cout << "PASSED\n";
}

void test_filesystem_missing_metadata_with_existing_blob() {
    std::cout << "Running test_filesystem_missing_metadata_with_existing_blob... ";
    std::string path_template = "/tmp/pisf-fs-missingmeta-test-XXXXXX";
    std::string temp_dir = ::mkdtemp(path_template.data());
    assert(!temp_dir.empty());

    // Create a data.blob but NO metadata.pisf
    std::string blob_path = temp_dir + "/data.blob";
    {
        std::ofstream out(blob_path, std::ios::binary);
        out << "some existing blob bytes";
    }

    pisf::Filesystem fs(temp_dir);
    assert(!fs.is_initialized());
    assert(fs.init_error().find("missing metadata") != std::string::npos);

    // Verify metadata.pisf was NOT silently created to overwrite or ignore data.blob
    assert(!std::filesystem::exists(temp_dir + "/metadata.pisf"));

    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::cout << "PASSED\n";
}

void test_filesystem_corrupted_metadata_rejected() {
    std::cout << "Running test_filesystem_corrupted_metadata_rejected... ";
    std::string path_template = "/tmp/pisf-fs-corruptmeta-test-XXXXXX";
    std::string temp_dir = ::mkdtemp(path_template.data());
    assert(!temp_dir.empty());

    std::string meta_path = temp_dir + "/metadata.pisf";
    std::string corrupted_content = "ThisIsNotAValidPisfMetadataBinaryFileContent!";
    {
        std::ofstream out(meta_path, std::ios::binary);
        out << corrupted_content;
    }

    pisf::Filesystem fs(temp_dir);
    assert(!fs.is_initialized());
    assert(!fs.init_error().empty());

    // Verify corrupted metadata was NOT deleted or overwritten
    std::ifstream in(meta_path, std::ios::binary);
    std::string remaining((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    assert(remaining == corrupted_content);

    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::cout << "PASSED\n";
}

void test_filesystem_invalid_extent_exceeding_blob() {
    std::cout << "Running test_filesystem_invalid_extent_exceeding_blob... ";
    std::string path_template = "/tmp/pisf-fs-badextent-test-XXXXXX";
    std::string temp_dir = ::mkdtemp(path_template.data());
    assert(!temp_dir.empty());

    // Create small data.blob (10 bytes)
    std::string blob_path = temp_dir + "/data.blob";
    {
        std::ofstream out(blob_path, std::ios::binary);
        out << "0123456789";
    }

    // Metadata specifies extent [20, 30), which exceeds blob length (10)
    pisf::MetadataState state;
    state.next_node_id = 3;

    pisf::Node root;
    root.id = 1;
    root.name = "/";
    root.type = pisf::NodeType::Directory;
    root.parent_id = std::nullopt;
    state.nodes[1] = root;

    pisf::Node file;
    file.id = 2;
    file.name = "bad.txt";
    file.type = pisf::NodeType::File;
    file.parent_id = 1;
    file.content_offset = 20;
    file.content_length = 10;
    state.nodes[2] = file;

    std::string err;
    assert(pisf::MetadataSerializer::save_to_file_atomic(temp_dir, state, err));

    pisf::Filesystem fs(temp_dir);
    assert(!fs.is_initialized());
    assert(fs.init_error().find("exceeds blob size") != std::string::npos);

    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::cout << "PASSED\n";
}

void test_filesystem_fresh_dir_creates_metadata_and_root() {
    std::cout << "Running test_filesystem_fresh_dir_creates_metadata_and_root... ";
    std::string path_template = "/tmp/pisf-fs-fresh-test-XXXXXX";
    std::string temp_dir = ::mkdtemp(path_template.data());
    assert(!temp_dir.empty());

    {
        pisf::Filesystem fs(temp_dir);
        assert(fs.is_initialized());
        assert(std::filesystem::exists(temp_dir + "/metadata.pisf"));
        assert(std::filesystem::exists(temp_dir + "/data.blob"));
        assert(fs.stat("/") == "name=/ type=directory id=1");
        assert(fs.total_nodes() == 1);
    }

    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::cout << "PASSED\n";
}

int main() {
    std::cout << "=== PISF In-Memory Filesystem Unit Tests ===\n";

    test_root_initialization();
    test_create_and_mkdir();
    test_listing_and_sorting();
    test_stat_metadata();
    test_missing_and_invalid_parents();
    test_duplicate_names();
    test_identical_names_in_different_dirs();
    test_path_normalization_and_validation();
    test_concurrent_same_path_creation();
    test_concurrent_independent_operations();
    test_read_empty_file();
    test_write_and_read_content();
    test_replace_content();
    test_content_containing_spaces();
    test_missing_file_read_write();
    test_directory_read_write();
    test_empty_content_and_newlines();
    test_concurrent_different_files();
    test_concurrent_same_file_read_write();
    test_storage_backed_write_extents();
    test_storage_backed_read_reflects_blob();
    test_filesystem_restore_from_metadata();
    test_filesystem_missing_metadata_with_existing_blob();
    test_filesystem_corrupted_metadata_rejected();
    test_filesystem_invalid_extent_exceeding_blob();
    test_filesystem_fresh_dir_creates_metadata_and_root();

    std::cout << "All filesystem unit tests passed successfully!\n";
    return 0;
}
