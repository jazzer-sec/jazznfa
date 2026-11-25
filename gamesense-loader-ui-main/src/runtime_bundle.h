#pragma once
#include <filesystem>

namespace runtime_bundle
{
    // Extract the embedded, source-built self-contained service on first launch.
    // Returns a per-user cache path; never requires files beside JazzNfa.exe.
    std::filesystem::path backend();
}
