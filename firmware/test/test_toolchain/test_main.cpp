#include <unity.h>

void setUp() {}
void tearDown() {}

// The core must stay within C++17: newer features would compile on the target
// (ESP-IDF defaults to a later standard) but break this contract.
void test_compiles_as_cpp17() {
    TEST_ASSERT_EQUAL(201703L, __cplusplus);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_compiles_as_cpp17);
    return UNITY_END();
}
