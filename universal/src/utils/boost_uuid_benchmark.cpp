#include <benchmark/benchmark.h>

#include <userver/utils/boost_uuid4.hpp>
#include <userver/utils/boost_uuid7.hpp>

USERVER_NAMESPACE_BEGIN

template <typename UuidGeneratorFunc>
void GenerateUuidSeries(UuidGeneratorFunc generator, benchmark::State& state) {
  for ([[maybe_unused]] auto _ : state) {
    for (int i = 0; i < state.range(0); ++i) {
      benchmark::DoNotOptimize(generator());
      benchmark::ClobberMemory();
    }
  }
}

void GenerateUuidV4Series(benchmark::State& state) {
  GenerateUuidSeries(&utils::generators::GenerateBoostUuid, state);
}

void GenerateUuidV7Series(benchmark::State& state) {
  GenerateUuidSeries(&utils::generators::GenerateBoostUuid7, state);
}

void GenerateUuidV7V2Series(benchmark::State& state) {
  GenerateUuidSeries(&utils::generators::GenerateBoostUuid7V2, state);
}

BENCHMARK(GenerateUuidV4Series)->RangeMultiplier(2)->Range(1, 1 << 20);
BENCHMARK(GenerateUuidV7Series)->RangeMultiplier(2)->Range(1, 1 << 20);
BENCHMARK(GenerateUuidV7V2Series)->RangeMultiplier(2)->Range(1, 1 << 20);

USERVER_NAMESPACE_END