#include <chrono>
#include <cstring>
#include <filesystem>

#include "Test.h"
#include "core/Platform.h"

int main(int argc, char** argv) {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / ("kinetic-tests-" + std::to_string(kinetic::platform::processId()));
    fs::create_directories(dir);
    ktest::tmpDir() = dir.string();

    const char* filter = argc > 1 ? argv[1] : nullptr;
    int passed = 0, failed = 0;
    for (auto& c : ktest::registry()) {
        if (filter && !std::strstr(c.name.c_str(), filter)) continue;
        auto t0 = std::chrono::steady_clock::now();
        try {
            c.fn();
            auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            std::cout << "  PASS  " << c.name << "  (" << static_cast<int>(ms) << " ms)\n";
            ++passed;
        } catch (const ktest::Failure& f) {
            std::cout << "  FAIL  " << c.name << "\n        " << f.msg << "\n";
            ++failed;
        } catch (const std::exception& e) {
            std::cout << "  FAIL  " << c.name << "\n        exception: " << e.what() << "\n";
            ++failed;
        }
    }
    std::cout << "\n" << passed << " passed, " << failed << " failed\n";
    std::error_code ec;
    fs::remove_all(dir, ec);
    return failed ? 1 : 0;
}
