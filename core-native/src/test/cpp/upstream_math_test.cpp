// Runs upstream correctness suites against the native core built for Android.
#include <cstdio>

bool TestGEMath();
bool TestSplineTessellation();

int main() {
    const bool math = TestGEMath();
    const bool spline = TestSplineTessellation();
    std::printf("Upstream GE arithmetic: %s\n", math ? "PASS" : "FAIL");
    std::printf("Upstream spline tessellation: %s\n", spline ? "PASS" : "FAIL");
    return math && spline ? 0 : 1;
}
