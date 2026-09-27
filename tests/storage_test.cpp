#include <pisf/storage.hpp>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

std::string create_test_temp_dir() {
    std::string path_template = "/tmp/pisf-storage-test-XXXXXX";
    std::string temp_dir = ::mkdtemp(path_template.data());
    assert(!temp_dir.empty());
    return temp_dir;
}

void cleanup_test_dir(const std::string& path) {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
}

void test_storage_init_creates_dir_and_blob() {
    std::cout << "Running test_storage_init_creates_dir_and_blob... ";
    std::string base_dir = create_test_temp_dir();
    std::string storage_dir = base_dir + "/sub/storage";

    pisf::BlobStorage storage(storage_dir, "data.blob");
    assert(!storage.is_open());

    std::string err;
    bool ok = storage.init(err);
    assert(ok);
    assert(err.empty());
    assert(storage.is_open());
    assert(storage.get_size() == 0);

    // Verify file exists on filesystem
    assert(std::filesystem::exists(storage.blob_path()));
    assert(std::filesystem::is_regular_file(storage.blob_path()));

    storage.close();
    assert(!storage.is_open());

    cleanup_test_dir(base_dir);
    std::cout << "PASSED\n";
}

void test_storage_init_existing_blob() {
    std::cout << "Running test_storage_init_existing_blob... ";
    std::string base_dir = create_test_temp_dir();

    // Create a pre-existing blob with some content
    std::string blob_file = base_dir + "/data.blob";
    {
        std::ofstream out(blob_file, std::ios::binary);
        out << "pre-existing content";
    }

    pisf::BlobStorage storage(base_dir, "data.blob");
    std::string err;
    assert(storage.init(err));
    assert(storage.is_open());
    assert(storage.get_size() == 20); // Length of "pre-existing content"

    storage.close();
    cleanup_test_dir(base_dir);
    std::cout << "PASSED\n";
}

void test_storage_init_error_handling() {
    std::cout << "Running test_storage_init_error_handling... ";
    std::string base_dir = create_test_temp_dir();

    // Create a regular file where the directory is expected
    std::string blocking_file = base_dir + "/file_blocker";
    {
        std::ofstream out(blocking_file);
        out << "not a dir";
    }

    // Attempt to initialize storage inside a path where a parent component is a file
    pisf::BlobStorage invalid_storage(blocking_file + "/child_dir", "data.blob");
    std::string err;
    bool ok = invalid_storage.init(err);
    assert(!ok);
    assert(!err.empty());
    assert(!invalid_storage.is_open());

    // Empty path test
    pisf::BlobStorage empty_storage("", "data.blob");
    assert(!empty_storage.init(err));
    assert(!err.empty());

    cleanup_test_dir(base_dir);
    std::cout << "PASSED\n";
}

void test_storage_move_semantics() {
    std::cout << "Running test_storage_move_semantics... ";
    std::string base_dir = create_test_temp_dir();

    pisf::BlobStorage storage1(base_dir, "data.blob");
    std::string err;
    assert(storage1.init(err));
    assert(storage1.is_open());

    pisf::BlobStorage storage2 = std::move(storage1);
    assert(!storage1.is_open());
    assert(storage2.is_open());
    assert(storage2.blob_path() == base_dir + "/data.blob");

    storage2.close();
    assert(!storage2.is_open());

    cleanup_test_dir(base_dir);
    std::cout << "PASSED\n";
}

} // namespace

int main() {
    std::cout << "=== PISF Persistent Storage Unit Tests ===\n";
    test_storage_init_creates_dir_and_blob();
    test_storage_init_existing_blob();
    test_storage_init_error_handling();
    test_storage_move_semantics();
    std::cout << "All persistent storage unit tests passed successfully!\n";
    return 0;
}
