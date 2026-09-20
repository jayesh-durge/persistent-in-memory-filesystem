#include <pisf/filesystem.hpp>

#include <atomic>
#include <cassert>
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

    std::cout << "All filesystem unit tests passed successfully!\n";
    return 0;
}
