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

void test_storage_append_and_read_single() {
    std::cout << "Running test_storage_append_and_read_single... ";
    std::string base_dir = create_test_temp_dir();

    pisf::BlobStorage storage(base_dir, "data.blob");
    std::string err;
    assert(storage.init(err));

    std::string payload = "Hello, Persistent World!";
    uint64_t offset = 999;
    uint64_t length = 999;
    assert(storage.append(payload, offset, length, err));
    assert(offset == 0);
    assert(length == payload.size());
    assert(storage.get_size() == payload.size());

    std::string read_back;
    assert(storage.read_range(offset, length, read_back, err));
    assert(read_back == payload);

    cleanup_test_dir(base_dir);
    std::cout << "PASSED\n";
}

void test_storage_append_and_read_multiple() {
    std::cout << "Running test_storage_append_and_read_multiple... ";
    std::string base_dir = create_test_temp_dir();

    pisf::BlobStorage storage(base_dir, "data.blob");
    std::string err;
    assert(storage.init(err));

    std::string rec1 = "FirstRecord";
    std::string rec2 = "SecondRecordWithMoreData";
    std::string rec3 = "ThirdRecordEnd";

    uint64_t off1 = 0, len1 = 0;
    uint64_t off2 = 0, len2 = 0;
    uint64_t off3 = 0, len3 = 0;

    assert(storage.append(rec1, off1, len1, err));
    assert(off1 == 0 && len1 == rec1.size());

    assert(storage.append(rec2, off2, len2, err));
    assert(off2 == len1 && len2 == rec2.size());

    assert(storage.append(rec3, off3, len3, err));
    assert(off3 == off2 + len2 && len3 == rec3.size());

    // Read back in arbitrary order
    std::string out;
    assert(storage.read_range(off2, len2, out, err));
    assert(out == rec2);

    assert(storage.read_range(off1, len1, out, err));
    assert(out == rec1);

    assert(storage.read_range(off3, len3, out, err));
    assert(out == rec3);

    // Read sub-range
    assert(storage.read_range(off2 + 6, 6, out, err));
    assert(out == "Record");

    cleanup_test_dir(base_dir);
    std::cout << "PASSED\n";
}

void test_storage_empty_content() {
    std::cout << "Running test_storage_empty_content... ";
    std::string base_dir = create_test_temp_dir();

    pisf::BlobStorage storage(base_dir, "data.blob");
    std::string err;
    assert(storage.init(err));

    uint64_t off = 999, len = 999;
    assert(storage.append("", off, len, err));
    assert(off == 0 && len == 0);
    assert(storage.get_size() == 0);

    std::string out = "initial";
    assert(storage.read_range(0, 0, out, err));
    assert(out.empty());

    // Append non-empty, then empty again
    assert(storage.append("data", off, len, err));
    assert(off == 0 && len == 4);

    assert(storage.append("", off, len, err));
    assert(off == 4 && len == 0);
    assert(storage.get_size() == 4);

    assert(storage.read_range(4, 0, out, err));
    assert(out.empty());

    cleanup_test_dir(base_dir);
    std::cout << "PASSED\n";
}

void test_storage_invalid_ranges_and_errors() {
    std::cout << "Running test_storage_invalid_ranges_and_errors... ";
    std::string base_dir = create_test_temp_dir();

    pisf::BlobStorage storage(base_dir, "data.blob");
    std::string err;
    assert(storage.init(err));

    uint64_t off = 0, len = 0;
    assert(storage.append("SampleData", off, len, err)); // size is 10

    std::string out;
    // Offset out of bounds
    assert(!storage.read_range(11, 1, out, err));
    assert(!err.empty());

    // Range extends past end of blob
    assert(!storage.read_range(5, 10, out, err));
    assert(!err.empty());

    // Offset + length overflow
    assert(!storage.read_range(UINT64_MAX - 2, 5, out, err));
    assert(!err.empty());

    // Closed storage operations
    storage.close();
    assert(!storage.append("more", off, len, err));
    assert(!storage.read_range(0, 5, out, err));

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
    test_storage_append_and_read_single();
    test_storage_append_and_read_multiple();
    test_storage_empty_content();
    test_storage_invalid_ranges_and_errors();
    std::cout << "All persistent storage unit tests passed successfully!\n";
    return 0;
}
