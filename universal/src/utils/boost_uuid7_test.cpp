#include <userver/utils/boost_uuid7.hpp>
#include <userver/utils/uuid7.hpp>

#include <algorithm>

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <gtest/gtest.h>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <boost/uuid/uuid_serialize.hpp>

USERVER_NAMESPACE_BEGIN

TEST(UUIDv7, Basic) {
  EXPECT_NE(utils::generators::GenerateBoostUuid7(), boost::uuids::uuid{});

  EXPECT_NE(utils::generators::GenerateBoostUuid7(),
            utils::generators::GenerateBoostUuid7());
}

TEST(UUIDv7, Ordered) {
  static constexpr auto kUuidsToGenerate = 1'000'000;

  std::vector<boost::uuids::uuid> uuids;
  uuids.reserve(kUuidsToGenerate);

  for (auto i = 0; i < kUuidsToGenerate; ++i) {
    uuids.push_back(utils::generators::GenerateBoostUuid7());
  }

  // sequentially generated uuids v7 should be ordered and unique
  for (size_t i = 0; i < uuids.size() - 1; ++i) {
    EXPECT_LT(uuids[i], uuids[i + 1])
        << "uuids[" << i << "]=" << uuids[i] << " should be less than uuids["
        << i + 1 << "]=" << uuids[i + 1];
  }
}

USERVER_NAMESPACE_END