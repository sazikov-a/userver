#include <sys/time.h>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>

#include <benchmark/benchmark.h>
#include <boost/random/uniform_int.hpp>
#include <boost/random/variate_generator.hpp>
#include <boost/uuid/uuid.hpp>

#include <userver/compiler/thread_local.hpp>
#include <userver/crypto/random.hpp>
#include <userver/utils/boost_uuid4.hpp>
#include <userver/utils/boost_uuid7.hpp>
#include <userver/utils/rand.hpp>
#include <userver/utils/span.hpp>

USERVER_NAMESPACE_BEGIN

namespace {

uint64_t ChronoTimestamp() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

uint64_t GettimeofdayTimestamp() {
  struct timeval tp {};
  gettimeofday(&tp, nullptr);

  return static_cast<uint64_t>(tp.tv_sec) * 1000 + tp.tv_usec / 1000;
}

uint64_t ClockRealtimeTimestamp() {
  ::timespec tp{};
  ::clock_gettime(CLOCK_REALTIME, &tp);

  return static_cast<uint64_t>(tp.tv_sec) * 1000 + tp.tv_nsec / 1000000;
}

uint64_t ClockRealtimeCoarseTimestamp() {
  ::timespec tp{};
  ::clock_gettime(CLOCK_REALTIME_COARSE, &tp);

  return static_cast<uint64_t>(tp.tv_sec) * 1000 + tp.tv_nsec / 1000000;
}

class UnbufferedWeakRandomGenerator {
 public:
  UnbufferedWeakRandomGenerator(utils::RandomBase& rng)
      : generator_(&rng, boost::uniform_int<uint64_t>(
                             std::numeric_limits<uint64_t>::min(),
                             std::numeric_limits<uint64_t>::max())) {}

  void operator()(utils::span<uint8_t> buffer) {
    int i = 0;
    uint64_t rnd_value = generator_();

    for (auto it = buffer.begin(), end = buffer.end(); it != end; ++it, ++i) {
      if (i == sizeof(uint64_t)) {
        rnd_value = generator_();
        i = 0;
      }

      *it = static_cast<uint8_t>((rnd_value >> (i * 8)) & 0xFF);
    }
  }

 private:
  boost::random::variate_generator<utils::RandomBase*,
                                   boost::uniform_int<uint64_t>>
      generator_;
};

class UnbufferedWeakRandomGeneratorV2 {
 public:
  UnbufferedWeakRandomGeneratorV2(utils::RandomBase& rng)
      : generator_(&rng, boost::uniform_int<uint64_t>(
                             std::numeric_limits<uint64_t>::min(),
                             std::numeric_limits<uint64_t>::max())) {}

  void operator()(utils::span<uint8_t> buffer) {
    int full_blocks = buffer.size() / sizeof(uint64_t);

    for (int i = 0; i < full_blocks; ++i) {
      const auto rnd_value = generator_();
      memcpy(buffer.begin() + i * sizeof(uint64_t), &rnd_value,
             sizeof(uint64_t));
    }

    uint64_t rnd_value = generator_();
    memcpy(buffer.begin() + full_blocks * sizeof(uint64_t), &rnd_value,
           buffer.size() - full_blocks * sizeof(uint64_t));
  }

 private:
  boost::random::variate_generator<utils::RandomBase*,
                                   boost::uniform_int<uint64_t>>
      generator_;
};

compiler::ThreadLocal local_unbuffered_weak_generator = [] {
  return utils::WithDefaultRandom([](utils::RandomBase& rng) {
    return UnbufferedWeakRandomGenerator(rng);
  });
};

compiler::ThreadLocal local_unbuffered_weak_generator_v2 = [] {
  return utils::WithDefaultRandom([](utils::RandomBase& rng) {
    return UnbufferedWeakRandomGeneratorV2(rng);
  });
};

void UnbufferedWeakRandom(utils::span<uint8_t> buffer) {
  auto gen = local_unbuffered_weak_generator.Use();

  (*gen)(buffer);
}

void UnbufferedWeakRandomV2(utils::span<uint8_t> buffer) {
  auto gen = local_unbuffered_weak_generator_v2.Use();

  (*gen)(buffer);
}

}  // namespace

template <typename RandomBlock>
void GenerateRandomBlockBase(RandomBlock func, benchmark::State& state) {
  for ([[maybe_unused]] auto _ : state) {
    state.PauseTiming();
    std::vector<uint8_t> buffer(state.range(0));
    state.ResumeTiming();

    func(utils::span<uint8_t>(buffer));
    benchmark::ClobberMemory();
  }
}

void GenerateRandomBlockUnbufferedWeak(benchmark::State& state) {
  GenerateRandomBlockBase(&UnbufferedWeakRandom, state);
}

void GenerateRandomBlockUnbufferedWeakV2(benchmark::State& state) {
  GenerateRandomBlockBase(&UnbufferedWeakRandomV2, state);
}

BENCHMARK(GenerateRandomBlockUnbufferedWeak)->DenseRange(1, 16);
BENCHMARK(GenerateRandomBlockUnbufferedWeakV2)->DenseRange(1, 16);

template <typename TimestampFunc>
void CurrentTimestampSeries(TimestampFunc func, benchmark::State& state) {
  for ([[maybe_unused]] auto _ : state) {
    for (int i = 0; i < state.range(0); ++i) {
      benchmark::DoNotOptimize(func());
    }
  }
}

void ChronoTimestampSeries(benchmark::State& state) {
  CurrentTimestampSeries(&ChronoTimestamp, state);
}

void GettimeofdayTimestampSeries(benchmark::State& state) {
  CurrentTimestampSeries(&GettimeofdayTimestamp, state);
}

void ClockRealtimeTimestampSeries(benchmark::State& state) {
  CurrentTimestampSeries(&ClockRealtimeTimestamp, state);
}

void ClockRealtimeCoarseTimestampSeries(benchmark::State& state) {
  CurrentTimestampSeries(&ClockRealtimeCoarseTimestamp, state);
}

BENCHMARK(ChronoTimestampSeries)->RangeMultiplier(2)->Range(1, 1 << 20);
BENCHMARK(GettimeofdayTimestampSeries)->RangeMultiplier(2)->Range(1, 1 << 20);
BENCHMARK(ClockRealtimeTimestampSeries)->RangeMultiplier(2)->Range(1, 1 << 20);
BENCHMARK(ClockRealtimeCoarseTimestampSeries)
    ->RangeMultiplier(2)
    ->Range(1, 1 << 20);

template <typename UuidGeneratorFunc>
void GenerateUuidSeries(UuidGeneratorFunc generator, benchmark::State& state) {
  for ([[maybe_unused]] auto _ : state) {
    for (int i = 0; i < state.range(0); ++i) {
      benchmark::DoNotOptimize(generator());
    }
  }
}

void GenerateUuidV4Series(benchmark::State& state) {
  GenerateUuidSeries(&utils::generators::GenerateBoostUuid, state);
}

void GenerateUuidV7Series(benchmark::State& state) {
  GenerateUuidSeries(&utils::generators::GenerateBoostUuid7, state);
}

BENCHMARK(GenerateUuidV4Series)->RangeMultiplier(2)->Range(1, 1 << 20);
BENCHMARK(GenerateUuidV7Series)->RangeMultiplier(2)->Range(1, 1 << 20);

USERVER_NAMESPACE_END