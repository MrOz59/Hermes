/**
 * @file tests/unit/test_thread_safe.cpp
 * @brief Shared-resource startup and lifetime regressions.
 */
#include <gtest/gtest.h>
#include <src/thread_safe.h>
#include <stdexcept>

TEST(SharedResourceTest, DestroysTheObjectWhenInitializationThrows) {
  int destructions = 0;
  int shutdowns = 0;
  bool fail = true;

  struct context_t {
    int *destructions = nullptr;

    ~context_t() {
      if (destructions) {
        ++*destructions;
      }
    }
  };

  auto shared = safe::make_shared<context_t>([&](context_t &context) {
    context.destructions = &destructions;
    if (fail) {
      throw std::runtime_error("startup failed");
    }
    return 0;
  },
                                             [&](context_t &) {
                                               ++shutdowns;
                                             });

  EXPECT_THROW(shared.ref(), std::runtime_error);
  EXPECT_EQ(destructions, 1);
  EXPECT_EQ(shutdowns, 0);
  fail = false;
  {
    auto reference = shared.ref();
    ASSERT_TRUE(static_cast<bool>(reference));
  }
  EXPECT_EQ(destructions, 2);
  EXPECT_EQ(shutdowns, 1);
}

TEST(SharedResourceTest, SupportsOverAlignedObjectsAndRepeatedLifetimes) {
  struct alignas(64) context_t {
    std::uint64_t value = 0;
  };

  int startups = 0;
  int shutdowns = 0;
  auto shared = safe::make_shared<context_t>([&](context_t &context) {
    context.value = ++startups;
    return 0;
  },
                                             [&](context_t &) {
                                               ++shutdowns;
                                             });
  for (int attempt = 1; attempt <= 20; ++attempt) {
    auto reference = shared.ref();
    ASSERT_TRUE(static_cast<bool>(reference));
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(reference.get()) % alignof(context_t), 0);
    EXPECT_EQ(reference->value, attempt);
  }
  EXPECT_EQ(startups, 20);
  EXPECT_EQ(shutdowns, 20);
}
