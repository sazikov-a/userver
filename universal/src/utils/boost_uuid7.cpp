#include <type_traits>
#include <userver/utils/boost_uuid7.hpp>

#include <endian.h>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <limits>

#include <boost/random/uniform_int.hpp>
#include <boost/random/variate_generator.hpp>
#include <boost/uuid/uuid.hpp>

#include <userver/compiler/thread_local.hpp>
#include <userver/utils/rand.hpp>
#include <userver/utils/span.hpp>

USERVER_NAMESPACE_BEGIN

namespace utils {

namespace generators {

namespace {

class UuidV7Generator {
  static_assert(std::is_same_v<uint8_t, boost::uuids::uuid::value_type>);

 public:
  UuidV7Generator(RandomBase& rng)
      : generator_(&rng, boost::uniform_int<uint64_t>(
                             std::numeric_limits<uint64_t>::min(),
                             std::numeric_limits<uint64_t>::max())) {}

  boost::uuids::uuid operator()() {
    boost::uuids::uuid uuid{};
    auto current_timestamp = CurrentUnixTimestamp();

    if (current_timestamp <= prev_timestamp_) {
      ++sequence_counter_;

      if (sequence_counter_ > kMaxSequenceCounterValue) {
        // We use 18 bits (12 bits from rand_a and 6 bits from rand_b) for
        // counter and in order to protect from rollover we will increment
        // timestamp ahead of the actual time. See section `Counter Rollover
        // Handling`
        // https://datatracker.ietf.org/doc/html/draft-ietf-uuidrev-rfc4122bis-09#monotonicity_counters

        sequence_counter_ = 0;
        current_timestamp = ++prev_timestamp_;
      }

      // fill var and rand_b with random data
      GenerateRandomBlock(utils::span<uint8_t>(uuid.data).subspan(8));

      // fill rand_a and rand_b with counter data

      // 4 most significant bits of 18-bit counter
      uuid.data[6] = static_cast<uint8_t>(sequence_counter_ >> 14);
      // next 8 bits
      uuid.data[7] = static_cast<uint8_t>(sequence_counter_ >> 6);
      // 6 least significant bits
      uuid.data[8] = static_cast<uint8_t>(sequence_counter_);
    } else {
      // fill ver, rand_a, var and rand_b with random data
      GenerateRandomBlock(utils::span<uint8_t>(uuid.data).subspan(6));

      // keep most significant bit of a counter initialized as zero
      // for guarding against counter rollover.
      // See section `Fixed-Length Dedicated Counter Seeding`
      // https://datatracker.ietf.org/doc/html/draft-ietf-uuidrev-rfc4122bis-09#monotonicity_counters
      uuid.data[6] &= 0xF7;

      sequence_counter_ = (static_cast<uint32_t>(uuid.data[6] & 0x0F) << 14) +
                          (static_cast<uint32_t>(uuid.data[7]) << 6) +
                          static_cast<uint32_t>(uuid.data[8] & 0x3F);
      prev_timestamp_ = current_timestamp;
    }

    // fill unix_ts_ms
    const auto be_shifted_timestamp = htobe64(current_timestamp << 16ull);
    memcpy(&uuid.data[0], &be_shifted_timestamp, 6);

    // fill ver (top four bits are 0, 1, 1, 1)
    uuid.data[6] = (uuid.data[6] & 0x0F) | 0x70;

    // fill var ( top two bits are 1, 0)
    uuid.data[8] = (uuid.data[8] & 0x3F) | 0x80;

    return uuid;
  }

 private:
  void GenerateRandomBlock(utils::span<uint8_t> block) {
    int i = 0;
    uint64_t rnd_value = generator_();

    for (auto it = block.begin(), end = block.end(); it != end; ++it, ++i) {
      if (i == sizeof(uint64_t)) {
        rnd_value = generator_();
        i = 0;
      }

      *it = static_cast<uint8_t>((rnd_value >> (i * 8)) & 0xFF);
    }
  }

  static uint64_t CurrentUnixTimestamp() {
    ::timespec tp{};
    ::clock_gettime(CLOCK_REALTIME, &tp);

    return static_cast<uint64_t>(tp.tv_sec) * 1000 + tp.tv_nsec / 1000000;
  }

 private:
  boost::random::variate_generator<RandomBase*, boost::uniform_int<uint64_t>>
      generator_;

  uint32_t sequence_counter_{0};
  uint64_t prev_timestamp_{0};

  static constexpr uint32_t kMaxSequenceCounterValue = 0x3FFFF;
};

class UuidV7GeneratorV2 {
  static_assert(std::is_same_v<uint8_t, boost::uuids::uuid::value_type>);

 public:
  UuidV7GeneratorV2(RandomBase& rng)
      : generator_(&rng, boost::uniform_int<uint64_t>(
                             std::numeric_limits<uint64_t>::min(),
                             std::numeric_limits<uint64_t>::max())) {}

  boost::uuids::uuid operator()() {
    boost::uuids::uuid uuid{};
    auto current_timestamp = CurrentUnixTimestamp();

    if (current_timestamp <= prev_timestamp_) {
      ++sequence_counter_;

      if (sequence_counter_ > kMaxSequenceCounterValue) {
        // We use 18 bits (12 bits from rand_a and 6 bits from rand_b) for
        // counter and in order to protect from rollover we will increment
        // timestamp ahead of the actual time. See section `Counter Rollover
        // Handling`
        // https://datatracker.ietf.org/doc/html/draft-ietf-uuidrev-rfc4122bis-09#monotonicity_counters

        sequence_counter_ = 0;
        current_timestamp = ++prev_timestamp_;
      }

      // fill var and rand_b with random data
      GenerateRandomBlock(utils::span<uint8_t>(uuid.data).subspan(8));

      // fill rand_a and rand_b with counter data

      // 4 most significant bits of 18-bit counter
      uuid.data[6] = static_cast<uint8_t>(sequence_counter_ >> 14);
      // next 8 bits
      uuid.data[7] = static_cast<uint8_t>(sequence_counter_ >> 6);
      // 6 least significant bits
      uuid.data[8] = static_cast<uint8_t>(sequence_counter_);
    } else {
      // fill ver, rand_a, var and rand_b with random data
      GenerateRandomBlock(utils::span<uint8_t>(uuid.data).subspan(6));

      // keep most significant bit of a counter initialized as zero
      // for guarding against counter rollover.
      // See section `Fixed-Length Dedicated Counter Seeding`
      // https://datatracker.ietf.org/doc/html/draft-ietf-uuidrev-rfc4122bis-09#monotonicity_counters
      uuid.data[6] &= 0xF7;

      sequence_counter_ = (static_cast<uint32_t>(uuid.data[6] & 0x0F) << 14) +
                          (static_cast<uint32_t>(uuid.data[7]) << 6) +
                          static_cast<uint32_t>(uuid.data[8] & 0x3F);
      prev_timestamp_ = current_timestamp;
    }

    // fill unix_ts_ms
    uuid.data[0] = static_cast<uint8_t>(current_timestamp >> 40);
    uuid.data[1] = static_cast<uint8_t>(current_timestamp >> 32);
    uuid.data[2] = static_cast<uint8_t>(current_timestamp >> 24);
    uuid.data[3] = static_cast<uint8_t>(current_timestamp >> 16);
    uuid.data[4] = static_cast<uint8_t>(current_timestamp >> 8);
    uuid.data[5] = static_cast<uint8_t>(current_timestamp);

    // fill ver (top four bits are 0, 1, 1, 1)
    uuid.data[6] = (uuid.data[6] & 0x0F) | 0x70;

    // fill var ( top two bits are 1, 0)
    uuid.data[8] = (uuid.data[8] & 0x3F) | 0x80;

    return uuid;
  }

 private:
  void GenerateRandomBlock(utils::span<uint8_t> block) {
    int i = 0;
    uint64_t rnd_value = generator_();

    for (auto it = block.begin(), end = block.end(); it != end; ++it, ++i) {
      if (i == sizeof(uint64_t)) {
        rnd_value = generator_();
        i = 0;
      }

      *it = static_cast<uint8_t>((rnd_value >> (i * 8)) & 0xFF);
    }
  }

  static uint64_t CurrentUnixTimestamp() {
    ::timespec tp{};
    ::clock_gettime(CLOCK_REALTIME, &tp);

    return static_cast<uint64_t>(tp.tv_sec) * 1000 + tp.tv_nsec / 1000000;
  }

 private:
  boost::random::variate_generator<RandomBase*, boost::uniform_int<uint64_t>>
      generator_;

  uint32_t sequence_counter_{0};
  uint64_t prev_timestamp_{0};

  static constexpr uint32_t kMaxSequenceCounterValue = 0x3FFFF;
};

compiler::ThreadLocal local_uuid7_generator = [] {
  return WithDefaultRandom(
      [](RandomBase& rng) { return UuidV7Generator(rng); });
};

compiler::ThreadLocal local_uuid7_generator_v2 = [] {
  return WithDefaultRandom(
      [](RandomBase& rng) { return UuidV7GeneratorV2(rng); });
};

}  // namespace

// Implementation is based on
// https://www.postgresql.org/message-id/flat/CAAhFRxitJv%3DyoGnXUgeLB_O%2BM7J2BJAmb5jqAT9gZ3bij3uLDA%40mail.gmail.com
boost::uuids::uuid GenerateBoostUuid7() {
  auto generator = local_uuid7_generator.Use();
  return (*generator)();
}

boost::uuids::uuid GenerateBoostUuid7V2() {
  auto generator = local_uuid7_generator_v2.Use();
  return (*generator)();
}

}  // namespace generators

}  // namespace utils

USERVER_NAMESPACE_END