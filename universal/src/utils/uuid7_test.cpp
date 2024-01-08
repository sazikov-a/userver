#include <userver/utils/uuid7.hpp>

#include <gtest/gtest.h>

USERVER_NAMESPACE_BEGIN

TEST(UUIDv7, String) {
  EXPECT_NE(utils::generators::GenerateUuid7(), "");

  constexpr unsigned kUuidMinLength = 32;
  EXPECT_GE(utils::generators::GenerateUuid7().size(), kUuidMinLength);

  EXPECT_NE(utils::generators::GenerateUuid7(),
            utils::generators::GenerateUuid7());
}

USERVER_NAMESPACE_END
