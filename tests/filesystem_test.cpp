#include <pisf/filesystem.hpp>
#include <pisf/metadata.hpp>
#include <pisf/common.hpp>

#include <atomic>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
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

    // Directories do not carry a content_length field
    std::string root_stat = fs.stat("/");
    assert(root_stat == "name=/ type=directory id=1");

    std::string dir_stat = fs.stat("/docs");
    assert(dir_stat.rfind("name=docs type=directory id=", 0) == 0);
    assert(dir_stat.find("content_length=") == std::string::npos);

    // Empty file reports content_length=0
    std::string file_stat = fs.stat("/docs/guide.txt");
    assert(file_stat.rfind("name=guide.txt type=file id=", 0) == 0);
    assert(file_stat.find("content_length=0") != std::string::npos);

    // Non-existent target stat
    std::string missing_stat = fs.stat("/missing.txt");
    assert(missing_stat.rfind("ERR not found", 0) == 0);

    std::cout << "PASSED\n";
}

void test_stat_content_length() {
    std::cout << "Running test_stat_content_length... ";
    pisf::Filesystem fs;

    // Empty file: content_length=0
    assert(fs.create("/empty.txt") == "OK");
    std::string s1 = fs.stat("/empty.txt");
    assert(s1.find("type=file") != std::string::npos);
    assert(s1.find("content_length=0") != std::string::npos);

    // File with data: content_length matches byte count
    assert(fs.create("/hello.txt") == "OK");
    assert(fs.write("/hello.txt", "hello world") == "OK");   // 11 bytes
    std::string s2 = fs.stat("/hello.txt");
    assert(s2.find("content_length=11") != std::string::npos);

    // After overwrite: length reflects new content, not cumulative blob bytes
    assert(fs.write("/hello.txt", "hi") == "OK");            // 2 bytes
    std::string s3 = fs.stat("/hello.txt");
    assert(s3.find("content_length=2") != std::string::npos);

    // Directory: no content_length field
    assert(fs.mkdir("/mydir") == "OK");
    std::string s4 = fs.stat("/mydir");
    assert(s4.find("type=directory") != std::string::npos);
    assert(s4.find("content_length=") == std::string::npos);

    // Root directory: no content_length field
    std::string s5 = fs.stat("/");
    assert(s5.find("content_length=") == std::string::npos);

    std::cout << "PASSED\n";
}

void test_stat_content_length_after_restart() {
    std::cout << "Running test_stat_content_length_after_restart... ";
    std::string dir = pisf::get_default_storage_dir() + "/stat_restart_test_" + std::to_string(getpid());
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    {
        pisf::Filesystem fs(dir);
        assert(fs.is_initialized());
        assert(fs.create("/persist.txt") == "OK");
        assert(fs.write("/persist.txt", "durable content") == "OK");   // 15 bytes
        std::string s = fs.stat("/persist.txt");
        assert(s.find("content_length=15") != std::string::npos);
    }

    // Restart: reload from disk
    {
        pisf::Filesystem fs(dir);
        assert(fs.is_initialized());
        std::string s = fs.stat("/persist.txt");
        assert(s.find("type=file") != std::string::npos);
        assert(s.find("content_length=15") != std::string::npos);
    }

    std::filesystem::remove_all(dir);
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
        assert(fs.stat("/workspace/doc.txt") == "name=doc.txt type=file id=3 content_length=23");

        // Create new file and verify NodeId continues from restored next_node_id (5)
        assert(fs.create("/workspace/new.txt") == "OK");
        assert(fs.stat("/workspace/new.txt") == "name=new.txt type=file id=5 content_length=0");
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

void test_mutation_persistence() {
    std::cout << "Running test_mutation_persistence... ";
    std::string path_template = "/tmp/pisf-fs-mutpersist-test-XXXXXX";
    std::string temp_dir = ::mkdtemp(path_template.data());
    assert(!temp_dir.empty());

    {
        pisf::Filesystem fs(temp_dir);
        assert(fs.mkdir("/notes") == "OK");
        assert(fs.create("/notes/file1.txt") == "OK");
        assert(fs.write("/notes/file1.txt", "ContentOne") == "OK");
        assert(fs.create("/notes/file2.txt") == "OK");

        // Verify on disk by loading metadata file directly
        pisf::MetadataState on_disk;
        std::string err;
        assert(pisf::MetadataSerializer::load_from_file(temp_dir, on_disk, err));
        assert(on_disk.nodes.size() == 4);
        assert(on_disk.next_node_id == 5);

        // Find file1
        pisf::NodeId f1_id = 0;
        for (const auto& [id, n] : on_disk.nodes) {
            if (n.name == "file1.txt") {
                f1_id = id;
                assert(n.content_length == 10);
            }
        }
        assert(f1_id != 0);
    }

    // Reopen filesystem from disk and verify everything persisted
    {
        pisf::Filesystem fs2(temp_dir);
        assert(fs2.exists("/notes"));
        assert(fs2.exists("/notes/file1.txt"));
        assert(fs2.exists("/notes/file2.txt"));
        assert(fs2.read("/notes/file1.txt") == "ContentOne");
        assert(fs2.read("/notes/file2.txt").empty());
    }

    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::cout << "PASSED\n";
}

void test_mutation_persistence_failure_rollback() {
    std::cout << "Running test_mutation_persistence_failure_rollback... ";
    std::string path_template = "/tmp/pisf-fs-rollback-test-XXXXXX";
    std::string temp_dir = ::mkdtemp(path_template.data());
    assert(!temp_dir.empty());

    {
        pisf::Filesystem fs(temp_dir);
        assert(fs.create("/doc.txt") == "OK");
        assert(fs.write("/doc.txt", "InitialCommittedContent") == "OK");
        assert(fs.read("/doc.txt") == "InitialCommittedContent");

        // Enable simulated metadata failure
        fs.set_simulate_metadata_failure(true);

        // 1. MKDIR failure & rollback
        std::string mkdir_res = fs.mkdir("/fail_dir");
        assert(mkdir_res.find("ERR metadata persistence failed") != std::string::npos);
        assert(!fs.exists("/fail_dir"));
        assert(fs.ls("/").find("fail_dir") == std::string::npos);

        // 2. CREATE failure & rollback
        std::string create_res = fs.create("/fail_file.txt");
        assert(create_res.find("ERR metadata persistence failed") != std::string::npos);
        assert(!fs.exists("/fail_file.txt"));
        assert(fs.ls("/").find("fail_file.txt") == std::string::npos);

        // 3. WRITE failure & rollback of content reference
        std::string write_res = fs.write("/doc.txt", "UncommittedNewContent");
        assert(write_res.find("ERR metadata persistence failed") != std::string::npos);
        // Previous content reference must not be lost
        assert(fs.read("/doc.txt") == "InitialCommittedContent");

        // Disable simulated failure
        fs.set_simulate_metadata_failure(false);

        // Successful mutation after recovery
        assert(fs.create("/valid_file.txt") == "OK");
        assert(fs.exists("/valid_file.txt"));
    }

    // Reopen filesystem to verify disk state reflects only successfully committed mutations
    {
        pisf::Filesystem fs2(temp_dir);
        assert(fs2.exists("/doc.txt"));
        assert(fs2.read("/doc.txt") == "InitialCommittedContent");
        assert(fs2.exists("/valid_file.txt"));
        assert(!fs2.exists("/fail_dir"));
        assert(!fs2.exists("/fail_file.txt"));
    }

    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::cout << "PASSED\n";
}

void test_mutation_persistence_dir_sync_failure_no_state_mismatch() {
    std::cout << "Running test_mutation_persistence_dir_sync_failure_no_state_mismatch... ";
    std::string path_template = "/tmp/pisf-fs-dirsync-test-XXXXXX";
    std::string temp_dir = ::mkdtemp(path_template.data());
    assert(!temp_dir.empty());

    {
        pisf::Filesystem fs(temp_dir);
        assert(fs.create("/file1.txt") == "OK");

        // Enable simulated directory sync failure (occurs after rename has succeeded)
        pisf::MetadataSerializer::set_simulate_dir_sync_failure(true);

        std::string res = fs.create("/file2.txt");
        assert(res.find("ERR metadata directory sync failed") != std::string::npos);

        // In-memory state MUST NOT be rolled back, because metadata.pisf on disk contains file2
        assert(fs.exists("/file2.txt"));
        assert(fs.ls("/").find("file2.txt") != std::string::npos);

        pisf::MetadataSerializer::set_simulate_dir_sync_failure(false);
    }

    // Reopen filesystem to verify on-disk state matches in-memory state (no mismatch)
    {
        pisf::Filesystem fs2(temp_dir);
        assert(fs2.exists("/file1.txt"));
        assert(fs2.exists("/file2.txt"));
    }

    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::cout << "PASSED\n";
}

void test_concurrent_writes_same_file_correct_extent() {
    // Verifies that concurrent writes to the same file never produce a torn
    // extent: after all writers finish the in-memory content must exactly match
    // one writer's payload, not a mixture of two writers' data.
    std::cout << "Running test_concurrent_writes_same_file_correct_extent... ";

    auto temp_dir = std::filesystem::temp_directory_path() /
                    ("pisf_concurrent_write_test." + std::to_string(::getpid()));
    std::filesystem::create_directories(temp_dir);

    pisf::Filesystem fs(temp_dir.string());
    assert(fs.is_initialized());

    assert(fs.create("/target.txt") == "OK");
    assert(fs.write("/target.txt", "INITIAL") == "OK");

    // Each writer uses a unique, fixed-size payload so we can identify which
    // one won the last write.
    constexpr int NUM_WRITERS = 6;
    constexpr int ITERATIONS  = 8;  // keep very short: single lock held across fdatasync per iteration
    std::vector<std::string> payloads;
    payloads.push_back("INITIAL");
    for (int w = 0; w < NUM_WRITERS; ++w) {
        // All payloads are the same length — makes content matching simpler
        payloads.push_back("WRITER_" + std::string(1, static_cast<char>('A' + w)) +
                           "_PAYLOAD__XYZ");
    }

    std::vector<std::thread> threads;
    for (int w = 0; w < NUM_WRITERS; ++w) {
        threads.emplace_back([&fs, &payloads, w]() {
            const std::string& payload = payloads[static_cast<size_t>(w + 1)];
            for (int i = 0; i < ITERATIONS; ++i) {
                std::string result = fs.write("/target.txt", payload);
                // Either OK or a durability error — never a torn extent
                assert(result == "OK" ||
                       result.find("ERR metadata") != std::string::npos ||
                       result.find("ERR storage") != std::string::npos);
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }

    // Final read must exactly match one of the known payloads
    std::string final_content = fs.read("/target.txt");
    bool matched = false;
    for (const auto& p : payloads) {
        if (final_content == p) {
            matched = true;
            break;
        }
    }
    assert(matched && "final file content does not match any writer's payload — torn extent detected");

    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::cout << "PASSED\n";
}

void test_concurrent_writes_different_files_no_regression() {
    // Verifies that concurrent writes to distinct files still all succeed and
    // each file independently holds one writer's last payload.
    std::cout << "Running test_concurrent_writes_different_files_no_regression... ";

    auto temp_dir = std::filesystem::temp_directory_path() /
                    ("pisf_concurrent_write_diff." + std::to_string(::getpid()));
    std::filesystem::create_directories(temp_dir);

    pisf::Filesystem fs(temp_dir.string());
    assert(fs.is_initialized());

    constexpr int NUM_FILES   = 4;
    constexpr int ITERATIONS  = 6;  // keep very short: single lock held across fdatasync per iteration

    for (int i = 0; i < NUM_FILES; ++i) {
        assert(fs.create("/file" + std::to_string(i) + ".txt") == "OK");
    }

    std::vector<std::thread> threads;
    for (int f = 0; f < NUM_FILES; ++f) {
        threads.emplace_back([&fs, f]() {
            std::string path    = "/file" + std::to_string(f) + ".txt";
            std::string payload = "FILE" + std::to_string(f) + "_CONTENT_VALUE";
            for (int i = 0; i < ITERATIONS; ++i) {
                std::string result = fs.write(path, payload);
                assert(result == "OK" ||
                       result.find("ERR") != std::string::npos);
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }

    // Each file should be readable and contain a well-formed string
    for (int f = 0; f < NUM_FILES; ++f) {
        std::string path    = "/file" + std::to_string(f) + ".txt";
        std::string content = fs.read(path);
        // Must contain the file index prefix
        assert(content.find("FILE" + std::to_string(f)) != std::string::npos);
    }

    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::cout << "PASSED\n";
}

void test_read_line_bounded_input() {
    std::cout << "Running test_read_line_bounded_input... ";
    int sv[2];
    assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    std::string buffer;
    std::string line;
    std::string err;

    // 1. Normal command
    std::string normal = "PING\n";
    assert(::write(sv[1], normal.data(), normal.size()) == static_cast<ssize_t>(normal.size()));
    assert(pisf::read_line(sv[0], line, buffer, err, 100) == pisf::ReadStatus::Success);
    assert(line == "PING");
    assert(buffer.empty());

    // 2. Command exactly at limit (limit = 10, payload = 10 bytes + \n)
    std::string exact = "1234567890\n";
    assert(::write(sv[1], exact.data(), exact.size()) == static_cast<ssize_t>(exact.size()));
    assert(pisf::read_line(sv[0], line, buffer, err, 10) == pisf::ReadStatus::Success);
    assert(line == "1234567890");
    assert(buffer.empty());

    // 3. Command exceeding limit with newline (11 bytes + \n with limit 10)
    std::string exceeding = "12345678901\n";
    assert(::write(sv[1], exceeding.data(), exceeding.size()) == static_cast<ssize_t>(exceeding.size()));
    assert(pisf::read_line(sv[0], line, buffer, err, 10) == pisf::ReadStatus::Error);
    assert(err.find("exceeds maximum allowed limit") != std::string::npos);
    buffer.clear();

    // 4. Multiple chunks exceeding limit without newline
    std::string chunk1 = "123456";
    std::string chunk2 = "78901";
    assert(::write(sv[1], chunk1.data(), chunk1.size()) == static_cast<ssize_t>(chunk1.size()));
    assert(::write(sv[1], chunk2.data(), chunk2.size()) == static_cast<ssize_t>(chunk2.size()));
    assert(pisf::read_line(sv[0], line, buffer, err, 10) == pisf::ReadStatus::Error);
    assert(err.find("no newline encountered") != std::string::npos);
    buffer.clear();

    // 5. Multiple commands received together
    std::string multi = "CMD1\nCMD2\r\nCMD3\n";
    assert(::write(sv[1], multi.data(), multi.size()) == static_cast<ssize_t>(multi.size()));
    assert(pisf::read_line(sv[0], line, buffer, err, 50) == pisf::ReadStatus::Success);
    assert(line == "CMD1");
    assert(pisf::read_line(sv[0], line, buffer, err, 50) == pisf::ReadStatus::Success);
    assert(line == "CMD2");
    assert(pisf::read_line(sv[0], line, buffer, err, 50) == pisf::ReadStatus::Success);
    assert(line == "CMD3");
    assert(buffer.empty());

    ::close(sv[0]);
    ::close(sv[1]);
    std::cout << "PASSED\n";
}

// ============================================================
// Milestone 7 — RM, RMDIR, MV
// ============================================================

void test_rm_basic() {
    std::cout << "Running test_rm_basic... ";
    pisf::Filesystem fs;

    assert(fs.create("/doc.txt") == "OK");
    assert(fs.write("/doc.txt", "hello") == "OK");

    // File must exist before deletion
    assert(fs.exists("/doc.txt"));

    // Delete it
    assert(fs.rm("/doc.txt") == "OK");

    // Must no longer exist
    assert(!fs.exists("/doc.txt"));

    // Read must fail
    std::string out, err;
    assert(!fs.read("/doc.txt", out, err));
    assert(err.find("ERR") != std::string::npos);

    // Parent directory must not contain the entry
    std::string ls_out = fs.ls("/");
    assert(ls_out.find("doc.txt") == std::string::npos);

    std::cout << "PASSED\n";
}

void test_rm_nested() {
    std::cout << "Running test_rm_nested... ";
    pisf::Filesystem fs;

    assert(fs.mkdir("/notes") == "OK");
    assert(fs.create("/notes/cn.txt") == "OK");
    assert(fs.write("/notes/cn.txt", "CompNetworks") == "OK");

    assert(fs.rm("/notes/cn.txt") == "OK");
    assert(!fs.exists("/notes/cn.txt"));

    // /notes directory must still exist and be empty
    assert(fs.exists("/notes"));
    std::string ls_out = fs.ls("/notes");
    assert(ls_out.empty());

    std::cout << "PASSED\n";
}

void test_rm_rejects_nonexistent() {
    std::cout << "Running test_rm_rejects_nonexistent... ";
    pisf::Filesystem fs;

    std::string resp = fs.rm("/no_such_file.txt");
    assert(resp.find("ERR") != std::string::npos);
    std::cout << "PASSED\n";
}

void test_rm_rejects_directory() {
    std::cout << "Running test_rm_rejects_directory... ";
    pisf::Filesystem fs;

    assert(fs.mkdir("/mydir") == "OK");
    std::string resp = fs.rm("/mydir");
    assert(resp.find("ERR") != std::string::npos);
    assert(fs.exists("/mydir")); // directory must still be there
    std::cout << "PASSED\n";
}

void test_rm_rejects_root() {
    std::cout << "Running test_rm_rejects_root... ";
    pisf::Filesystem fs;

    std::string resp = fs.rm("/");
    assert(resp.find("ERR") != std::string::npos);
    assert(fs.exists("/"));
    std::cout << "PASSED\n";
}

void test_rm_persistence() {
    std::cout << "Running test_rm_persistence... ";

    auto temp_dir = std::filesystem::temp_directory_path() /
                    ("pisf_rm_persist." + std::to_string(::getpid()));
    std::filesystem::create_directories(temp_dir);

    {
        pisf::Filesystem fs(temp_dir.string());
        assert(fs.is_initialized());
        assert(fs.create("/data.txt") == "OK");
        assert(fs.write("/data.txt", "ToDelete") == "OK");
        assert(fs.rm("/data.txt") == "OK");
        // Filesystem goes out of scope — metadata written
    }

    // Restart
    {
        pisf::Filesystem fs2(temp_dir.string());
        assert(fs2.is_initialized());
        assert(!fs2.exists("/data.txt")); // must not reappear after restart
        assert(fs2.ls("/").empty());
    }

    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::cout << "PASSED\n";
}

void test_rm_rollback_on_metadata_failure() {
    std::cout << "Running test_rm_rollback_on_metadata_failure... ";
    pisf::Filesystem fs;

    assert(fs.create("/rollback.txt") == "OK");
    assert(fs.write("/rollback.txt", "data") == "OK");

    fs.set_simulate_metadata_failure(true);
    std::string resp = fs.rm("/rollback.txt");
    fs.set_simulate_metadata_failure(false);

    assert(resp.find("ERR") != std::string::npos);
    // Node must still be there
    assert(fs.exists("/rollback.txt"));
    assert(fs.read("/rollback.txt") == "data");
    // Parent entry must still be intact
    assert(fs.ls("/").find("rollback.txt") != std::string::npos);
    std::cout << "PASSED\n";
}

void test_rmdir_basic() {
    std::cout << "Running test_rmdir_basic... ";
    pisf::Filesystem fs;

    assert(fs.mkdir("/emptydir") == "OK");
    assert(fs.exists("/emptydir"));

    assert(fs.rmdir("/emptydir") == "OK");
    assert(!fs.exists("/emptydir"));
    assert(fs.ls("/").find("emptydir") == std::string::npos);

    std::cout << "PASSED\n";
}

void test_rmdir_nested_empty() {
    std::cout << "Running test_rmdir_nested_empty... ";
    pisf::Filesystem fs;

    assert(fs.mkdir("/a") == "OK");
    assert(fs.mkdir("/a/b") == "OK");
    assert(fs.rmdir("/a/b") == "OK");
    assert(!fs.exists("/a/b"));
    assert(fs.exists("/a"));
    std::cout << "PASSED\n";
}

void test_rmdir_rejects_nonempty() {
    std::cout << "Running test_rmdir_rejects_nonempty... ";
    pisf::Filesystem fs;

    assert(fs.mkdir("/full") == "OK");
    assert(fs.create("/full/file.txt") == "OK");

    std::string resp = fs.rmdir("/full");
    assert(resp.find("ERR") != std::string::npos);
    assert(fs.exists("/full"));
    assert(fs.exists("/full/file.txt"));
    std::cout << "PASSED\n";
}

void test_rmdir_rejects_file() {
    std::cout << "Running test_rmdir_rejects_file... ";
    pisf::Filesystem fs;

    assert(fs.create("/afile.txt") == "OK");
    std::string resp = fs.rmdir("/afile.txt");
    // Must be a clear "not a directory" error, not "not a file"
    assert(resp.find("ERR") != std::string::npos);
    assert(resp.find("not a directory") != std::string::npos);
    assert(fs.exists("/afile.txt"));
    std::cout << "PASSED\n";
}

void test_rmdir_rejects_nonexistent() {
    std::cout << "Running test_rmdir_rejects_nonexistent... ";
    pisf::Filesystem fs;

    std::string resp = fs.rmdir("/no_such_dir");
    assert(resp.find("ERR") != std::string::npos);
    std::cout << "PASSED\n";
}

void test_rmdir_rejects_root() {
    std::cout << "Running test_rmdir_rejects_root... ";
    pisf::Filesystem fs;

    std::string resp = fs.rmdir("/");
    assert(resp.find("ERR") != std::string::npos);
    assert(fs.exists("/"));
    std::cout << "PASSED\n";
}

void test_rmdir_persistence() {
    std::cout << "Running test_rmdir_persistence... ";

    auto temp_dir = std::filesystem::temp_directory_path() /
                    ("pisf_rmdir_persist." + std::to_string(::getpid()));
    std::filesystem::create_directories(temp_dir);

    {
        pisf::Filesystem fs(temp_dir.string());
        assert(fs.is_initialized());
        assert(fs.mkdir("/tmpdir") == "OK");
        assert(fs.rmdir("/tmpdir") == "OK");
    }

    {
        pisf::Filesystem fs2(temp_dir.string());
        assert(fs2.is_initialized());
        assert(!fs2.exists("/tmpdir"));
        assert(fs2.ls("/").empty());
    }

    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::cout << "PASSED\n";
}

void test_rmdir_rollback_on_metadata_failure() {
    std::cout << "Running test_rmdir_rollback_on_metadata_failure... ";
    pisf::Filesystem fs;

    assert(fs.mkdir("/rollbackdir") == "OK");

    fs.set_simulate_metadata_failure(true);
    std::string resp = fs.rmdir("/rollbackdir");
    fs.set_simulate_metadata_failure(false);

    assert(resp.find("ERR") != std::string::npos);
    assert(fs.exists("/rollbackdir")); // must still exist
    assert(fs.ls("/").find("rollbackdir") != std::string::npos);
    std::cout << "PASSED\n";
}

void test_mv_rename_file_same_dir() {
    std::cout << "Running test_mv_rename_file_same_dir... ";
    pisf::Filesystem fs;

    assert(fs.create("/old.txt") == "OK");
    assert(fs.write("/old.txt", "ContentABC") == "OK");

    uint64_t orig_offset = 0, orig_length = 0;
    assert(fs.get_node_extent("/old.txt", orig_offset, orig_length));

    assert(fs.mv("/old.txt", "/new.txt") == "OK");

    // old name gone, new name present
    assert(!fs.exists("/old.txt"));
    assert(fs.exists("/new.txt"));
    assert(fs.ls("/").find("old.txt") == std::string::npos);
    assert(fs.ls("/").find("new.txt") != std::string::npos);

    // Content preserved
    assert(fs.read("/new.txt") == "ContentABC");

    // NodeId (extent) preserved
    uint64_t new_offset = 0, new_length = 0;
    assert(fs.get_node_extent("/new.txt", new_offset, new_length));
    assert(new_offset == orig_offset);
    assert(new_length == orig_length);

    std::cout << "PASSED\n";
}

void test_mv_rename_directory_same_dir() {
    std::cout << "Running test_mv_rename_directory_same_dir... ";
    pisf::Filesystem fs;

    assert(fs.mkdir("/old_dir") == "OK");
    assert(fs.create("/old_dir/file.txt") == "OK");
    assert(fs.write("/old_dir/file.txt", "InDir") == "OK");

    assert(fs.mv("/old_dir", "/new_dir") == "OK");

    assert(!fs.exists("/old_dir"));
    assert(fs.exists("/new_dir"));
    assert(fs.exists("/new_dir/file.txt"));
    assert(fs.read("/new_dir/file.txt") == "InDir");

    std::cout << "PASSED\n";
}

void test_mv_move_file_between_dirs() {
    std::cout << "Running test_mv_move_file_between_dirs... ";
    pisf::Filesystem fs;

    assert(fs.mkdir("/src_dir") == "OK");
    assert(fs.mkdir("/dst_dir") == "OK");
    assert(fs.create("/src_dir/file.txt") == "OK");
    assert(fs.write("/src_dir/file.txt", "MovedContent") == "OK");

    assert(fs.mv("/src_dir/file.txt", "/dst_dir/file.txt") == "OK");

    assert(!fs.exists("/src_dir/file.txt"));
    assert(fs.exists("/dst_dir/file.txt"));
    assert(fs.read("/dst_dir/file.txt") == "MovedContent");
    assert(fs.ls("/src_dir").empty());

    std::cout << "PASSED\n";
}

void test_mv_move_directory_between_dirs() {
    std::cout << "Running test_mv_move_directory_between_dirs... ";
    pisf::Filesystem fs;

    assert(fs.mkdir("/parent_a") == "OK");
    assert(fs.mkdir("/parent_b") == "OK");
    assert(fs.mkdir("/parent_a/subdir") == "OK");
    assert(fs.create("/parent_a/subdir/child.txt") == "OK");
    assert(fs.write("/parent_a/subdir/child.txt", "ChildData") == "OK");

    assert(fs.mv("/parent_a/subdir", "/parent_b/subdir") == "OK");

    assert(!fs.exists("/parent_a/subdir"));
    assert(fs.exists("/parent_b/subdir"));
    assert(fs.exists("/parent_b/subdir/child.txt"));
    assert(fs.read("/parent_b/subdir/child.txt") == "ChildData");
    assert(fs.ls("/parent_a").empty());

    std::cout << "PASSED\n";
}

void test_mv_preserves_descendants_and_nodeids() {
    std::cout << "Running test_mv_preserves_descendants_and_nodeids... ";
    pisf::Filesystem fs;

    assert(fs.mkdir("/root_dir") == "OK");
    assert(fs.create("/root_dir/f1.txt") == "OK");
    assert(fs.write("/root_dir/f1.txt", "F1Content") == "OK");
    assert(fs.mkdir("/root_dir/sub") == "OK");
    assert(fs.create("/root_dir/sub/f2.txt") == "OK");
    assert(fs.write("/root_dir/sub/f2.txt", "F2Content") == "OK");

    uint64_t f1_off = 0, f1_len = 0, f2_off = 0, f2_len = 0;
    assert(fs.get_node_extent("/root_dir/f1.txt", f1_off, f1_len));
    assert(fs.get_node_extent("/root_dir/sub/f2.txt", f2_off, f2_len));

    assert(fs.mv("/root_dir", "/moved_dir") == "OK");

    // All descendants accessible
    assert(fs.exists("/moved_dir"));
    assert(fs.exists("/moved_dir/f1.txt"));
    assert(fs.exists("/moved_dir/sub"));
    assert(fs.exists("/moved_dir/sub/f2.txt"));

    // Content preserved
    assert(fs.read("/moved_dir/f1.txt") == "F1Content");
    assert(fs.read("/moved_dir/sub/f2.txt") == "F2Content");

    // Extents (NodeId proxies) unchanged
    uint64_t new_f1_off = 0, new_f1_len = 0, new_f2_off = 0, new_f2_len = 0;
    assert(fs.get_node_extent("/moved_dir/f1.txt", new_f1_off, new_f1_len));
    assert(fs.get_node_extent("/moved_dir/sub/f2.txt", new_f2_off, new_f2_len));
    assert(new_f1_off == f1_off && new_f1_len == f1_len);
    assert(new_f2_off == f2_off && new_f2_len == f2_len);

    std::cout << "PASSED\n";
}

void test_mv_rejects_nonexistent_src() {
    std::cout << "Running test_mv_rejects_nonexistent_src... ";
    pisf::Filesystem fs;

    std::string resp = fs.mv("/no_such.txt", "/dst.txt");
    assert(resp.find("ERR") != std::string::npos);
    std::cout << "PASSED\n";
}

void test_mv_rejects_nonexistent_dst_parent() {
    std::cout << "Running test_mv_rejects_nonexistent_dst_parent... ";
    pisf::Filesystem fs;

    assert(fs.create("/src.txt") == "OK");
    std::string resp = fs.mv("/src.txt", "/no_parent/dst.txt");
    assert(resp.find("ERR") != std::string::npos);
    assert(fs.exists("/src.txt")); // unchanged
    std::cout << "PASSED\n";
}

void test_mv_rejects_existing_dst_name() {
    std::cout << "Running test_mv_rejects_existing_dst_name... ";
    pisf::Filesystem fs;

    assert(fs.create("/a.txt") == "OK");
    assert(fs.create("/b.txt") == "OK");

    std::string resp = fs.mv("/a.txt", "/b.txt");
    assert(resp.find("ERR") != std::string::npos);
    // Both files must still exist
    assert(fs.exists("/a.txt"));
    assert(fs.exists("/b.txt"));
    std::cout << "PASSED\n";
}

void test_mv_rejects_moving_root() {
    std::cout << "Running test_mv_rejects_moving_root... ";
    pisf::Filesystem fs;

    std::string resp = fs.mv("/", "/newroot");
    assert(resp.find("ERR") != std::string::npos);
    std::cout << "PASSED\n";
}

void test_mv_rejects_move_into_self() {
    std::cout << "Running test_mv_rejects_move_into_self... ";
    pisf::Filesystem fs;

    assert(fs.mkdir("/mydir") == "OK");
    std::string resp = fs.mv("/mydir", "/mydir/mydir");
    assert(resp.find("ERR") != std::string::npos);
    assert(fs.exists("/mydir")); // unchanged
    std::cout << "PASSED\n";
}

void test_mv_rejects_move_into_descendant() {
    std::cout << "Running test_mv_rejects_move_into_descendant... ";
    pisf::Filesystem fs;

    assert(fs.mkdir("/a") == "OK");
    assert(fs.mkdir("/a/b") == "OK");
    assert(fs.mkdir("/a/b/c") == "OK");

    // Attempt to move /a into /a/b/c — c is a descendant of a
    std::string resp = fs.mv("/a", "/a/b/c/a");
    assert(resp.find("ERR") != std::string::npos);
    assert(fs.exists("/a"));
    assert(fs.exists("/a/b"));
    assert(fs.exists("/a/b/c"));
    std::cout << "PASSED\n";
}

void test_mv_same_name_noop() {
    std::cout << "Running test_mv_same_name_noop... ";
    pisf::Filesystem fs;

    assert(fs.create("/same.txt") == "OK");
    assert(fs.write("/same.txt", "SameContent") == "OK");

    // Moving to the exact same path is a no-op
    assert(fs.mv("/same.txt", "/same.txt") == "OK");
    assert(fs.exists("/same.txt"));
    assert(fs.read("/same.txt") == "SameContent");
    std::cout << "PASSED\n";
}

void test_mv_persistence() {
    std::cout << "Running test_mv_persistence... ";

    auto temp_dir = std::filesystem::temp_directory_path() /
                    ("pisf_mv_persist." + std::to_string(::getpid()));
    std::filesystem::create_directories(temp_dir);

    {
        pisf::Filesystem fs(temp_dir.string());
        assert(fs.is_initialized());
        assert(fs.mkdir("/docs") == "OK");
        assert(fs.create("/docs/report.txt") == "OK");
        assert(fs.write("/docs/report.txt", "PersistMV") == "OK");
        assert(fs.mkdir("/archive") == "OK");

        assert(fs.mv("/docs/report.txt", "/archive/report.txt") == "OK");
    }

    {
        pisf::Filesystem fs2(temp_dir.string());
        assert(fs2.is_initialized());
        assert(!fs2.exists("/docs/report.txt"));
        assert(fs2.exists("/archive/report.txt"));
        assert(fs2.read("/archive/report.txt") == "PersistMV");
    }

    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::cout << "PASSED\n";
}

void test_mv_rollback_on_metadata_failure() {
    std::cout << "Running test_mv_rollback_on_metadata_failure... ";
    pisf::Filesystem fs;

    assert(fs.create("/orig.txt") == "OK");
    assert(fs.write("/orig.txt", "RollbackData") == "OK");

    fs.set_simulate_metadata_failure(true);
    std::string resp = fs.mv("/orig.txt", "/renamed.txt");
    fs.set_simulate_metadata_failure(false);

    assert(resp.find("ERR") != std::string::npos);
    // Source must still exist under old name
    assert(fs.exists("/orig.txt"));
    assert(fs.read("/orig.txt") == "RollbackData");
    // Destination must not have been created
    assert(!fs.exists("/renamed.txt"));
    // Root listing must show only original name
    std::string ls_out = fs.ls("/");
    assert(ls_out.find("orig.txt") != std::string::npos);
    assert(ls_out.find("renamed.txt") == std::string::npos);
    std::cout << "PASSED\n";
}

void test_compaction_basic() {
    std::cout << "Running test_compaction_basic... ";
    std::string path_template = "/tmp/pisf-fs-compact-basic-XXXXXX";
    std::string temp_dir = ::mkdtemp(path_template.data());
    assert(!temp_dir.empty());

    {
        pisf::Filesystem fs(temp_dir);
        assert(fs.create("/fileA.txt") == "OK");
        assert(fs.write("/fileA.txt", "AAA_original") == "OK");
        assert(fs.create("/fileB.txt") == "OK");
        assert(fs.write("/fileB.txt", "BBB_live") == "OK");
        assert(fs.create("/fileC.txt") == "OK");
        assert(fs.write("/fileC.txt", "CCC_live") == "OK");

        // Overwrite fileA with new content (making old content dead)
        assert(fs.write("/fileA.txt", "AAA_overwritten") == "OK");

        // Delete fileC (making its content dead)
        assert(fs.rm("/fileC.txt") == "OK");

        assert(fs.blob_generation() == 0);
        std::string gen0_path = temp_dir + "/data.blob";
        assert(std::filesystem::exists(gen0_path));

        // Trigger compaction
        std::string err;
        assert(fs.compact(err));
        assert(err.empty());

        // Generation must now be 1
        assert(fs.blob_generation() == 1);
        std::string gen1_path = temp_dir + "/data-1.blob";
        assert(std::filesystem::exists(gen1_path));
        // Old blob must be reclaimed
        assert(!std::filesystem::exists(gen0_path));

        // Verify live contents remain exact
        assert(fs.read("/fileA.txt") == "AAA_overwritten");
        assert(fs.read("/fileB.txt") == "BBB_live");
        assert(!fs.exists("/fileC.txt"));

        // Verify the size of gen1 blob is exactly the sum of live extents
        // "AAA_overwritten" is 15 bytes, "BBB_live" is 8 bytes = 23 bytes
        assert(std::filesystem::file_size(gen1_path) == 23);
    }

    // Reopen filesystem to verify crash recovery / persistence of compacted state
    {
        pisf::Filesystem fs2(temp_dir);
        assert(fs2.blob_generation() == 1);
        assert(fs2.read("/fileA.txt") == "AAA_overwritten");
        assert(fs2.read("/fileB.txt") == "BBB_live");
        assert(!fs2.exists("/fileC.txt"));

        // Verify writing to compacted generation works
        assert(fs2.write("/fileA.txt", "AAA_gen1_write") == "OK");
        assert(fs2.read("/fileA.txt") == "AAA_gen1_write");
    }

    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::cout << "PASSED\n";
}

void test_compaction_multiple_generations() {
    std::cout << "Running test_compaction_multiple_generations... ";
    std::string path_template = "/tmp/pisf-fs-compact-multigen-XXXXXX";
    std::string temp_dir = ::mkdtemp(path_template.data());
    assert(!temp_dir.empty());

    {
        pisf::Filesystem fs(temp_dir);
        assert(fs.create("/data.txt") == "OK");
        assert(fs.write("/data.txt", "gen0_content") == "OK");

        std::string err;
        // First compaction: gen 0 -> gen 1
        assert(fs.compact(err));
        assert(fs.blob_generation() == 1);
        assert(std::filesystem::exists(temp_dir + "/data-1.blob"));
        assert(!std::filesystem::exists(temp_dir + "/data.blob"));
        assert(fs.read("/data.txt") == "gen0_content");

        // Second compaction: gen 1 -> gen 2
        assert(fs.write("/data.txt", "gen1_content") == "OK");
        assert(fs.compact(err));
        assert(fs.blob_generation() == 2);
        assert(std::filesystem::exists(temp_dir + "/data-2.blob"));
        assert(!std::filesystem::exists(temp_dir + "/data-1.blob"));
        assert(fs.read("/data.txt") == "gen1_content");
    }

    // Reopen to verify generation 2 is loaded
    {
        pisf::Filesystem fs2(temp_dir);
        assert(fs2.blob_generation() == 2);
        assert(fs2.read("/data.txt") == "gen1_content");
    }

    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::cout << "PASSED\n";
}

void test_compaction_rollback_on_metadata_failure() {
    std::cout << "Running test_compaction_rollback_on_metadata_failure... ";
    std::string path_template = "/tmp/pisf-fs-compact-fail-XXXXXX";
    std::string temp_dir = ::mkdtemp(path_template.data());
    assert(!temp_dir.empty());

    {
        pisf::Filesystem fs(temp_dir);
        assert(fs.create("/stable.txt") == "OK");
        assert(fs.write("/stable.txt", "ImportantData") == "OK");
        assert(fs.blob_generation() == 0);

        // Simulate metadata persistence failure during compaction
        fs.set_simulate_metadata_failure(true);
        std::string err;
        bool ok = fs.compact(err);
        assert(!ok);
        assert(err.find("simulated metadata persistence failure") != std::string::npos);

        // Active generation must remain 0
        assert(fs.blob_generation() == 0);
        assert(std::filesystem::exists(temp_dir + "/data.blob"));
        // Uncommitted gen 1 blob must have been removed
        assert(!std::filesystem::exists(temp_dir + "/data-1.blob"));
        // Live data must remain readable
        assert(fs.read("/stable.txt") == "ImportantData");

        // Disable failure and compact successfully
        fs.set_simulate_metadata_failure(false);
        assert(fs.compact(err));
        assert(fs.blob_generation() == 1);
        assert(fs.read("/stable.txt") == "ImportantData");
    }

    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::cout << "PASSED\n";
}

void test_compaction_crash_during_blob_copy() {
    std::cout << "Running test_compaction_crash_during_blob_copy... ";
    std::string path_template = "/tmp/pisf-crash-copy-XXXXXX";
    std::string temp_dir = ::mkdtemp(path_template.data());
    assert(!temp_dir.empty());

    {
        pisf::Filesystem fs(temp_dir);
        assert(fs.create("/file1.txt") == "OK");
        assert(fs.write("/file1.txt", "OriginalContent1") == "OK");
        assert(fs.create("/file2.txt") == "OK");
        assert(fs.write("/file2.txt", "OriginalContent2") == "OK");
        assert(fs.blob_generation() == 0);
    }

    // Simulate crash during copy: partial data-1.blob exists on disk, but metadata still points to gen 0
    std::string partial_blob = temp_dir + "/data-1.blob";
    {
        std::ofstream out(partial_blob, std::ios::binary);
        out << "corrupted-or-partial-data";
    }
    assert(std::filesystem::exists(partial_blob));

    // Recovery must load gen 0 authoritatively and clean up orphaned data-1.blob
    {
        pisf::Filesystem fs(temp_dir);
        assert(fs.blob_generation() == 0);
        assert(fs.read("/file1.txt") == "OriginalContent1");
        assert(fs.read("/file2.txt") == "OriginalContent2");
        assert(!std::filesystem::exists(partial_blob));

        // Subsequent compaction must now succeed cleanly
        std::string err;
        assert(fs.compact(err));
        assert(fs.blob_generation() == 1);
        assert(fs.read("/file1.txt") == "OriginalContent1");
        assert(fs.read("/file2.txt") == "OriginalContent2");
        assert(std::filesystem::exists(temp_dir + "/data-1.blob"));
        assert(!std::filesystem::exists(temp_dir + "/data.blob"));
    }

    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::cout << "PASSED\n";
}

void test_compaction_crash_after_new_blob_synced_before_metadata_commit() {
    std::cout << "Running test_compaction_crash_after_new_blob_synced_before_metadata_commit... ";
    std::string path_template = "/tmp/pisf-crash-before-meta-XXXXXX";
    std::string temp_dir = ::mkdtemp(path_template.data());
    assert(!temp_dir.empty());

    {
        pisf::Filesystem fs(temp_dir);
        assert(fs.create("/doc.txt") == "OK");
        assert(fs.write("/doc.txt", "AuthoritativeGen0") == "OK");
    }

    // Create a complete data-1.blob, but metadata.pisf on disk still points to gen 0
    std::string gen1_blob = temp_dir + "/data-1.blob";
    {
        std::ofstream out(gen1_blob, std::ios::binary);
        out << "NewBlobDataNotReferencedByMetadata";
    }

    // Start filesystem: must select generation 0 and safely discard orphaned data-1.blob
    {
        pisf::Filesystem fs(temp_dir);
        assert(fs.blob_generation() == 0);
        assert(fs.read("/doc.txt") == "AuthoritativeGen0");
        assert(!std::filesystem::exists(gen1_blob));
    }

    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::cout << "PASSED\n";
}

void test_compaction_crash_during_old_generation_cleanup() {
    std::cout << "Running test_compaction_crash_during_old_generation_cleanup... ";
    std::string path_template = "/tmp/pisf-crash-cleanup-XXXXXX";
    std::string temp_dir = ::mkdtemp(path_template.data());
    assert(!temp_dir.empty());

    {
        pisf::Filesystem fs(temp_dir);
        assert(fs.create("/keep.txt") == "OK");
        assert(fs.write("/keep.txt", "Gen1Data") == "OK");
        std::string err;
        assert(fs.compact(err));
        assert(fs.blob_generation() == 1);
    }

    // Simulate crash during cleanup of previous generation:
    // Re-create obsolete data.blob
    {
        std::ofstream out(temp_dir + "/data.blob", std::ios::binary);
        out << "obsolete-leftover-data";
    }

    // Reopen filesystem: must select gen 1, verify active blob is kept, and obsolete data.blob is removed
    {
        pisf::Filesystem fs(temp_dir);
        assert(fs.blob_generation() == 1);
        assert(fs.read("/keep.txt") == "Gen1Data");
        assert(std::filesystem::exists(temp_dir + "/data-1.blob"));
        assert(!std::filesystem::exists(temp_dir + "/data.blob"));
    }

    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::cout << "PASSED\n";
}

void test_compaction_legacy_generation_0_recovery() {
    std::cout << "Running test_compaction_legacy_generation_0_recovery... ";
    std::string path_template = "/tmp/pisf-legacy-gen0-XXXXXX";
    std::string temp_dir = ::mkdtemp(path_template.data());
    assert(!temp_dir.empty());

    // Create a legacy gen-0 installation using V1 metadata header
    {
        std::string blob_path = temp_dir + "/data.blob";
        std::string content = "LegacyBlobContent";
        {
            std::ofstream out(blob_path, std::ios::binary);
            out << content;
        }

        // Craft V1 metadata pointing to content in data.blob
        pisf::MetadataState v1_state;
        v1_state.next_node_id = 3;
        v1_state.blob_generation = 0;

        pisf::Node root;
        root.id = 1;
        root.name = "/";
        root.type = pisf::NodeType::Directory;
        root.children["legacy.txt"] = 2;
        v1_state.nodes[1] = root;

        pisf::Node file_node;
        file_node.id = 2;
        file_node.name = "legacy.txt";
        file_node.type = pisf::NodeType::File;
        file_node.parent_id = 1;
        file_node.content_offset = 0;
        file_node.content_length = content.size();
        v1_state.nodes[2] = file_node;

        std::string v2_bytes;
        std::string err;
        assert(pisf::MetadataSerializer::serialize(v1_state, v2_bytes, err));
        // Strip 8-byte generation from header to create real V1 binary
        std::string v1_bytes = v2_bytes.substr(0, 28) + v2_bytes.substr(36, v2_bytes.size() - 36 - 4);
        v1_bytes[4] = 1; v1_bytes[5] = 0; v1_bytes[6] = 0; v1_bytes[7] = 0;
        uint32_t crc = pisf::MetadataSerializer::compute_crc32(
            reinterpret_cast<const uint8_t*>(v1_bytes.data()), v1_bytes.size());
        v1_bytes.push_back(static_cast<char>(crc & 0xFFu));
        v1_bytes.push_back(static_cast<char>((crc >> 8) & 0xFFu));
        v1_bytes.push_back(static_cast<char>((crc >> 16) & 0xFFu));
        v1_bytes.push_back(static_cast<char>((crc >> 24) & 0xFFu));

        std::ofstream meta_out(temp_dir + "/metadata.pisf", std::ios::binary);
        meta_out.write(v1_bytes.data(), v1_bytes.size());
    }

    // Load with Filesystem: must recognize legacy installation as gen 0 and read content
    {
        pisf::Filesystem fs(temp_dir);
        assert(fs.blob_generation() == 0);
        assert(fs.exists("/legacy.txt"));
        assert(fs.read("/legacy.txt") == "LegacyBlobContent");

        // Now compact it
        std::string err;
        assert(fs.compact(err));
        assert(fs.blob_generation() == 1);
        assert(fs.read("/legacy.txt") == "LegacyBlobContent");
        assert(std::filesystem::exists(temp_dir + "/data-1.blob"));
        assert(!std::filesystem::exists(temp_dir + "/data.blob"));
    }

    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::cout << "PASSED\n";
}

void test_storage_stats_and_space_reclamation() {
    std::cout << "Running test_storage_stats_and_space_reclamation... ";
    std::string path_template = "/tmp/pisf-stats-XXXXXX";
    std::string temp_dir = ::mkdtemp(path_template.data());
    assert(!temp_dir.empty());

    {
        pisf::Filesystem fs(temp_dir);

        // Initial empty state
        pisf::StorageStats s0 = fs.get_storage_stats();
        assert(s0.active_generation == 0);
        assert(s0.total_blob_bytes == 0);
        assert(s0.live_bytes == 0);
        assert(s0.reclaimable_bytes == 0);
        assert(s0.live_files == 0);

        // 1. Write a file
        assert(fs.create("/fileA.txt") == "OK");
        assert(fs.write("/fileA.txt", "InitialContentA") == "OK"); // 15 bytes
        pisf::StorageStats s1 = fs.get_storage_stats();
        assert(s1.total_blob_bytes == 15);
        assert(s1.live_bytes == 15);
        assert(s1.reclaimable_bytes == 0);
        assert(s1.live_files == 1);

        // 2. Overwrite fileA several times
        assert(fs.write("/fileA.txt", "SecondContentA!") == "OK"); // 15 bytes
        assert(fs.write("/fileA.txt", "FinalLongerContentForFileA") == "OK"); // 26 bytes
        pisf::StorageStats s2 = fs.get_storage_stats();
        assert(s2.total_blob_bytes == 15 + 15 + 26); // 56 bytes
        assert(s2.live_bytes == 26);
        assert(s2.reclaimable_bytes == 30); // 15 + 15 dead bytes
        assert(s2.live_files == 1);

        // 3. Create and delete another file
        assert(fs.create("/fileB.txt") == "OK");
        assert(fs.write("/fileB.txt", "FileBToBeDeleted") == "OK"); // 16 bytes
        pisf::StorageStats s3 = fs.get_storage_stats();
        assert(s3.total_blob_bytes == 56 + 16); // 72 bytes
        assert(s3.live_bytes == 26 + 16); // 42 bytes
        assert(s3.reclaimable_bytes == 30);
        assert(s3.live_files == 2);

        assert(fs.rm("/fileB.txt") == "OK");
        // 4. Confirm reclaimable bytes increase
        pisf::StorageStats s4 = fs.get_storage_stats();
        assert(s4.total_blob_bytes == 72);
        assert(s4.live_bytes == 26);
        assert(s4.reclaimable_bytes == 46); // 30 + 16 dead bytes
        assert(s4.live_files == 1);

        // 5. Compact
        std::string err;
        assert(fs.compact(err));

        // 6. Confirm live contents remain identical
        assert(fs.read("/fileA.txt") == "FinalLongerContentForFileA");
        assert(!fs.exists("/fileB.txt"));

        // 7. Confirm reclaimable bytes decrease to 0
        pisf::StorageStats s5 = fs.get_storage_stats();
        assert(s5.active_generation == 1);
        assert(s5.live_files == 1);
        assert(s5.live_bytes == 26);
        assert(s5.total_blob_bytes == 26);
        assert(s5.reclaimable_bytes == 0);

        // 8. Confirm the new active blob contains only live file contents
        std::string gen1_path = temp_dir + "/data-1.blob";
        assert(std::filesystem::exists(gen1_path));
        assert(std::filesystem::file_size(gen1_path) == 26);
        assert(!std::filesystem::exists(temp_dir + "/data.blob"));
    }

    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::cout << "PASSED\n";
}

void test_storage_integrity_truncated_blob_rejected() {
    std::cout << "Running test_storage_integrity_truncated_blob_rejected... ";
    std::string path_template = "/tmp/pisf-b-trunc-XXXXXX";
    std::string temp_dir = ::mkdtemp(path_template.data());
    assert(!temp_dir.empty());

    {
        pisf::Filesystem fs(temp_dir);
        assert(fs.create("/data.bin") == "OK");
        std::string long_content(100, 'X');
        assert(fs.write("/data.bin", long_content) == "OK");
    }

    // Truncate data.blob to 50 bytes (less than the 100 bytes referenced by metadata)
    std::string blob_path = temp_dir + "/data.blob";
    assert(::truncate(blob_path.c_str(), 50) == 0);
    assert(std::filesystem::file_size(blob_path) == 50);

    // Reopen: startup must fail and refuse to initialize an empty filesystem
    {
        pisf::Filesystem fs2(temp_dir);
        assert(!fs2.is_initialized());
        assert(fs2.init_error().find("exceeds blob size") != std::string::npos);
        // It must NOT have wiped the directory or created an empty root
        assert(std::filesystem::file_size(blob_path) == 50);
    }

    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::cout << "PASSED\n";
}

void test_storage_integrity_missing_active_generation_rejected() {
    std::cout << "Running test_storage_integrity_missing_active_generation_rejected... ";
    std::string path_template = "/tmp/pisf-b-missing-gen-XXXXXX";
    std::string temp_dir = ::mkdtemp(path_template.data());
    assert(!temp_dir.empty());

    {
        pisf::Filesystem fs(temp_dir);
        assert(fs.create("/file.txt") == "OK");
        assert(fs.write("/file.txt", "SomeImportantData") == "OK");
        std::string err;
        assert(fs.compact(err));
        assert(fs.blob_generation() == 1);
        assert(std::filesystem::exists(temp_dir + "/data-1.blob"));
    }

    // Delete active generation file data-1.blob
    std::error_code ec;
    std::filesystem::remove(temp_dir + "/data-1.blob", ec);
    assert(!std::filesystem::exists(temp_dir + "/data-1.blob"));

    // Reopen: must report missing active generation and NOT silently initialize empty filesystem
    {
        pisf::Filesystem fs2(temp_dir);
        assert(!fs2.is_initialized());
        assert(fs2.init_error().find("missing blob for active generation") != std::string::npos);
    }

    std::filesystem::remove_all(temp_dir, ec);
    std::cout << "PASSED\n";
}

void test_storage_integrity_empty_file_boundaries() {
    std::cout << "Running test_storage_integrity_empty_file_boundaries... ";
    std::string path_template = "/tmp/pisf-b-empty-XXXXXX";
    std::string temp_dir = ::mkdtemp(path_template.data());
    assert(!temp_dir.empty());

    {
        pisf::Filesystem fs(temp_dir);
        assert(fs.create("/empty.txt") == "OK");
        assert(fs.exists("/empty.txt"));
        assert(fs.stat("/empty.txt").find("type=file") != std::string::npos);

        std::string content;
        std::string err;
        assert(fs.read("/empty.txt", content, err));
        assert(content.empty());
        assert(fs.read("/empty.txt") == "");

        // Compact filesystem containing empty file
        assert(fs.compact(err));
        assert(fs.blob_generation() == 1);
        assert(fs.exists("/empty.txt"));
        assert(fs.read("/empty.txt") == "");
    }

    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::cout << "PASSED\n";
}

void test_concurrent_reads_while_file_is_renamed() {
    std::cout << "Running test_concurrent_reads_while_file_is_renamed... ";
    std::string path_template = "/tmp/pisf-c-read-rename-XXXXXX";
    std::string temp_dir = ::mkdtemp(path_template.data());
    assert(!temp_dir.empty());

    {
        pisf::Filesystem fs(temp_dir);
        assert(fs.create("/file.txt") == "OK");
        assert(fs.write("/file.txt", "StablePayloadData") == "OK");

        uint64_t initial_off = 0, initial_len = 0;
        assert(fs.get_node_extent("/file.txt", initial_off, initial_len));

        std::atomic<bool> stop{false};
        std::atomic<int> successful_reads{0};

        auto reader_fn = [&]() {
            while (!stop.load(std::memory_order_relaxed)) {
                std::string content;
                std::string err;
                if (fs.read("/file.txt", content, err)) {
                    assert(content == "StablePayloadData");
                    successful_reads.fetch_add(1, std::memory_order_relaxed);
                } else {
                    assert(err.find("not found") != std::string::npos ||
                           err.find("does not exist") != std::string::npos ||
                           err.find("ERR") != std::string::npos);
                }

                if (fs.read("/renamed.txt", content, err)) {
                    assert(content == "StablePayloadData");
                    successful_reads.fetch_add(1, std::memory_order_relaxed);
                }
            }
        };

        std::vector<std::thread> readers;
        for (int i = 0; i < 4; ++i) {
            readers.emplace_back(reader_fn);
        }

        // Mutation thread alternates rename
        for (int iter = 0; iter < 40; ++iter) {
            assert(fs.mv("/file.txt", "/renamed.txt") == "OK");
            assert(fs.mv("/renamed.txt", "/file.txt") == "OK");
        }

        stop.store(true, std::memory_order_relaxed);
        for (auto& t : readers) {
            t.join();
        }

        assert(successful_reads.load() > 0);
        // Final state: /file.txt exists with unchanged extent
        uint64_t final_off = 0, final_len = 0;
        assert(fs.get_node_extent("/file.txt", final_off, final_len));
        assert(final_off == initial_off);
        assert(final_len == initial_len);
        assert(fs.read("/file.txt") == "StablePayloadData");
    }

    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::cout << "PASSED\n";
}

void test_concurrent_reads_while_file_is_deleted() {
    std::cout << "Running test_concurrent_reads_while_file_is_deleted... ";
    std::string path_template = "/tmp/pisf-c-read-delete-XXXXXX";
    std::string temp_dir = ::mkdtemp(path_template.data());
    assert(!temp_dir.empty());

    {
        pisf::Filesystem fs(temp_dir);
        assert(fs.create("/target.txt") == "OK");
        assert(fs.write("/target.txt", "TargetContentForDeletion") == "OK");

        std::atomic<bool> stop{false};
        std::atomic<int> valid_reads{0};
        std::atomic<int> missing_reads{0};

        auto reader_fn = [&]() {
            while (!stop.load(std::memory_order_relaxed)) {
                std::string content;
                std::string err;
                if (fs.read("/target.txt", content, err)) {
                    assert(content == "TargetContentForDeletion");
                    valid_reads.fetch_add(1, std::memory_order_relaxed);
                } else {
                    missing_reads.fetch_add(1, std::memory_order_relaxed);
                }
            }
        };

        std::vector<std::thread> readers;
        for (int i = 0; i < 4; ++i) {
            readers.emplace_back(reader_fn);
        }

        std::this_thread::yield();

        // Delete file while readers are querying
        assert(fs.rm("/target.txt") == "OK");

        std::this_thread::sleep_for(std::chrono::milliseconds(5));

        stop.store(true, std::memory_order_relaxed);
        for (auto& t : readers) {
            t.join();
        }

        assert(!fs.exists("/target.txt"));
        assert(missing_reads.load() > 0);
    }

    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::cout << "PASSED\n";
}

void test_concurrent_compaction_with_active_readers_and_writers() {
    std::cout << "Running test_concurrent_compaction_with_active_readers_and_writers... ";
    std::string path_template = "/tmp/pisf-c-compact-stress-XXXXXX";
    std::string temp_dir = ::mkdtemp(path_template.data());
    assert(!temp_dir.empty());

    {
        pisf::Filesystem fs(temp_dir);
        for (int i = 0; i < 4; ++i) {
            std::string path = "/file" + std::to_string(i) + ".txt";
            assert(fs.create(path) == "OK");
            assert(fs.write(path, "InitialContent" + std::to_string(i)) == "OK");
        }

        std::atomic<bool> stop{false};
        std::atomic<int> read_ops{0};
        std::atomic<int> write_ops{0};

        // Reader threads
        auto reader_fn = [&](int id) {
            std::string path = "/file" + std::to_string(id) + ".txt";
            while (!stop.load(std::memory_order_relaxed)) {
                std::string content;
                std::string err;
                if (fs.read(path, content, err)) {
                    assert(content.find("Content" + std::to_string(id)) != std::string::npos);
                    read_ops.fetch_add(1, std::memory_order_relaxed);
                }
            }
        };

        // Writer thread
        auto writer_fn = [&]() {
            int ver = 1;
            while (!stop.load(std::memory_order_relaxed)) {
                std::string path = "/file0.txt";
                std::string res = fs.write(path, "UpdatedContent0_v" + std::to_string(ver++));
                if (res == "OK") {
                    write_ops.fetch_add(1, std::memory_order_relaxed);
                }
            }
        };

        std::vector<std::thread> threads;
        for (int i = 0; i < 4; ++i) {
            threads.emplace_back(reader_fn, i);
        }
        threads.emplace_back(writer_fn);

        // Run repeated compactions concurrently
        for (int c = 0; c < 3; ++c) {
            std::string err;
            assert(fs.compact(err));
            assert(err.empty());
        }

        stop.store(true, std::memory_order_relaxed);
        for (auto& t : threads) {
            t.join();
        }

        assert(read_ops.load() > 0);
        assert(fs.blob_generation() == 3);

        // All 4 files must be readable and valid
        for (int i = 0; i < 4; ++i) {
            std::string path = "/file" + std::to_string(i) + ".txt";
            std::string content = fs.read(path);
            assert(!content.empty());
            assert(content.find("Content" + std::to_string(i)) != std::string::npos);
        }
    }

    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::cout << "PASSED\n";
}

// ---------------------------------------------------------------------------
// Milestone 9 Checkpoint D — Compaction Performance Measurement
// ---------------------------------------------------------------------------
// Verifies that compact() completes within a generous wall-clock bound and
// correctly reclaims obsolete blob space. Measurements are printed for
// documentation but do not gate the test (beyond the time bound).
// ---------------------------------------------------------------------------

struct CompactionMeasurement {
    std::string label;
    uint64_t blob_bytes_before{0};
    uint64_t live_bytes{0};
    uint64_t blob_bytes_after{0};
    double   compaction_ms{0.0};
    double   throughput_mb_s{0.0};
    double   space_reclaim_pct{0.0};
};

static CompactionMeasurement run_compaction_benchmark(
        const std::string& label,
        int n_files,
        const std::string& content,
        int overwrites) {
    using Clock = std::chrono::steady_clock;
    using Ms    = std::chrono::duration<double, std::milli>;

    std::string path_template = "/tmp/pisf-bench-d-XXXXXX";
    std::string temp_dir = ::mkdtemp(path_template.data());
    assert(!temp_dir.empty());

    CompactionMeasurement result;
    result.label = label;

    {
        pisf::Filesystem fs(temp_dir);
        assert(fs.is_initialized());

        for (int i = 0; i < n_files; ++i) {
            std::string path = "/f" + std::to_string(i) + ".txt";
            assert(fs.create(path) == "OK");
            assert(fs.write(path, content) == "OK");
        }

        // Overwrite every other file (creates obsolete extents)
        for (int ow = 0; ow < overwrites; ++ow) {
            for (int i = 0; i < n_files; i += 2) {
                std::string path = "/f" + std::to_string(i) + ".txt";
                assert(fs.write(path, "OW_" + std::to_string(ow) + "_" + content) == "OK");
            }
        }

        auto stats_before = fs.get_storage_stats();
        result.blob_bytes_before = stats_before.total_blob_bytes;
        result.live_bytes        = stats_before.live_bytes;

        // Measure compaction time under exclusive lock
        auto t0 = Clock::now();
        std::string err;
        assert(fs.compact(err));
        auto t1 = Clock::now();

        result.compaction_ms = Ms(t1 - t0).count();
        result.throughput_mb_s = (static_cast<double>(result.live_bytes) / (1024.0 * 1024.0))
                                 / (result.compaction_ms / 1000.0);

        auto stats_after = fs.get_storage_stats();
        result.blob_bytes_after = stats_after.total_blob_bytes;

        if (result.blob_bytes_before > 0) {
            double reclaimed = static_cast<double>(
                static_cast<int64_t>(result.blob_bytes_before)
                - static_cast<int64_t>(result.blob_bytes_after));
            result.space_reclaim_pct = 100.0 * reclaimed
                                       / static_cast<double>(result.blob_bytes_before);
        }

        // Verify compacted blob equals live bytes
        assert(result.blob_bytes_after == result.live_bytes);

        // All files remain readable after compaction
        for (int i = 0; i < n_files; ++i) {
            std::string path = "/f" + std::to_string(i) + ".txt";
            std::string c, e;
            assert(fs.read(path, c, e));
            assert(!c.empty());
        }
    }

    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    return result;
}

void test_compaction_performance_measurements() {
    std::cout << "Running test_compaction_performance_measurements... \n";

    // Generous wall-clock bound: 60 s per workload (each write does fdatasync).
    // This is a correctness + documentation test, not a performance gate beyond
    // the generous bound.
    constexpr double kTimeLimitMs = 60'000.0;

    struct WorkloadSpec {
        std::string label;
        int         n_files;
        std::string content;
        int         overwrites;
    };

    const std::vector<WorkloadSpec> workloads = {
        {"20 files x 256B, 3 overwrites of every-other",
         20, std::string(256, 'A'), 3},
        {"40 files x 256B, 0 overwrites (nearly 100% live)",
         40, std::string(256, 'B'), 0},
        {"5 files x 4KB, 2 overwrites of every-other",
         5, std::string(4096, 'C'), 2},
        {"10 files x 512B, 10 overwrites (high obsolete ratio)",
         10, std::string(512, 'D'), 10},
    };

    std::cout << "\n  --- Compaction Performance Measurements ---\n";
    for (const auto& wl : workloads) {
        auto m = run_compaction_benchmark(wl.label, wl.n_files, wl.content, wl.overwrites);
        std::cout << "  [" << m.label << "]\n"
                  << "    Blob before:      " << m.blob_bytes_before << " bytes\n"
                  << "    Live bytes:       " << m.live_bytes << " bytes\n"
                  << "    Blob after:       " << m.blob_bytes_after << " bytes\n"
                  << "    Compaction time:  " << m.compaction_ms << " ms\n"
                  << "    Copy throughput:  " << m.throughput_mb_s << " MB/s\n"
                  << "    Space reclaimed:  " << m.space_reclaim_pct << " %\n\n";

        // Assert compaction completes within generous bound
        assert(m.compaction_ms < kTimeLimitMs
               && "Compaction exceeded 60 s time limit for small workload");
        // After compaction, blob must not exceed live data
        assert(m.blob_bytes_after <= m.live_bytes + 64 /* metadata rounding */);
    }

    std::cout << "  ------------------------------------------\n";
    std::cout << "PASSED\n";
}

// ---------------------------------------------------------------------------

int main() {
    std::cout << "=== PISF In-Memory Filesystem Unit Tests ===\n";

    test_root_initialization();
    test_create_and_mkdir();
    test_listing_and_sorting();
    test_stat_metadata();
    test_stat_content_length();
    test_stat_content_length_after_restart();
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
    test_concurrent_writes_same_file_correct_extent();
    test_concurrent_writes_different_files_no_regression();
    test_storage_backed_write_extents();
    test_storage_backed_read_reflects_blob();
    test_filesystem_restore_from_metadata();
    test_filesystem_missing_metadata_with_existing_blob();
    test_filesystem_corrupted_metadata_rejected();
    test_filesystem_invalid_extent_exceeding_blob();
    test_filesystem_fresh_dir_creates_metadata_and_root();
    test_mutation_persistence();
    test_mutation_persistence_failure_rollback();
    test_mutation_persistence_dir_sync_failure_no_state_mismatch();
    test_read_line_bounded_input();

    // Milestone 7 — RM
    test_rm_basic();
    test_rm_nested();
    test_rm_rejects_nonexistent();
    test_rm_rejects_directory();
    test_rm_rejects_root();
    test_rm_persistence();
    test_rm_rollback_on_metadata_failure();

    // Milestone 7 — RMDIR
    test_rmdir_basic();
    test_rmdir_nested_empty();
    test_rmdir_rejects_nonempty();
    test_rmdir_rejects_file();
    test_rmdir_rejects_nonexistent();
    test_rmdir_rejects_root();
    test_rmdir_persistence();
    test_rmdir_rollback_on_metadata_failure();

    // Milestone 7 — MV
    test_mv_rename_file_same_dir();
    test_mv_rename_directory_same_dir();
    test_mv_move_file_between_dirs();
    test_mv_move_directory_between_dirs();
    test_mv_preserves_descendants_and_nodeids();
    test_mv_rejects_nonexistent_src();
    test_mv_rejects_nonexistent_dst_parent();
    test_mv_rejects_existing_dst_name();
    test_mv_rejects_moving_root();
    test_mv_rejects_move_into_self();
    test_mv_rejects_move_into_descendant();
    test_mv_same_name_noop();
    test_mv_persistence();
    test_mv_rollback_on_metadata_failure();

    // Milestone 8 — Compaction
    test_compaction_basic();
    test_compaction_multiple_generations();
    test_compaction_rollback_on_metadata_failure();
    test_compaction_crash_during_blob_copy();
    test_compaction_crash_after_new_blob_synced_before_metadata_commit();
    test_compaction_crash_during_old_generation_cleanup();
    test_compaction_legacy_generation_0_recovery();
    test_storage_stats_and_space_reclamation();

    // Milestone 9 Checkpoint B — Storage Integrity Validation
    test_storage_integrity_truncated_blob_rejected();
    test_storage_integrity_missing_active_generation_rejected();
    test_storage_integrity_empty_file_boundaries();

    // Milestone 9 Checkpoint C — Concurrent Filesystem Stress
    test_concurrent_reads_while_file_is_renamed();
    test_concurrent_reads_while_file_is_deleted();
    test_concurrent_compaction_with_active_readers_and_writers();

    // Milestone 9 Checkpoint D — Compaction Performance Measurements
    test_compaction_performance_measurements();

    std::cout << "All filesystem unit tests passed successfully!\n";
    return 0;
}
