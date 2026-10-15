// Runs upstream correctness suites against the native core built for Android.
#include <cstdio>

bool TestCrossSIMD();
bool TestGEMath();
bool TestSplineTessellation();
bool TestBoundingBoxFast();
bool TestDepthRaster();

int main() {
    const bool simd = TestCrossSIMD();
    const bool math = TestGEMath();
    const bool spline = TestSplineTessellation();
    std::printf("Upstream SIMD operations: %s\n", simd ? "PASS" : "FAIL");
    std::printf("Upstream GE arithmetic: %s\n", math ? "PASS" : "FAIL");
    std::printf("Upstream spline tessellation: %s\n", spline ? "PASS" : "FAIL");
    const bool bounds = TestBoundingBoxFast();
    const bool depth = TestDepthRaster();
    std::printf("Upstream bounding box SIMD: %s\n", bounds ? "PASS" : "FAIL");
    std::printf("Upstream depth rasterization: %s\n", depth ? "PASS" : "FAIL");
    return simd && math && spline && bounds && depth ? 0 : 1;
}
