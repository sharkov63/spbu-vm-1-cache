#include "fmt/chrono.h"
#include "fmt/format.h"
#include <chrono>
#include <iostream>
#include <unistd.h>

static constexpr int PageSizeBytes = 4096;
static constexpr int MaxTestRegionSizeBytes = 128 * 1024 * 1024;
static constexpr int MeasureIters = 1024 * 1024;
static constexpr int MaxWaySize = 1024 * 1024;
static constexpr double JumpFactor = 1.3;

alignas(1024 * 1024) char TestRegion[MaxTestRegionSizeBytes];

static auto now() { return std::chrono::high_resolution_clock::now(); }

size_t Dummy;

static void preparePointerChain(char *Region, int RegionSize, int Stride,
                                int Count) {
  if (Stride < sizeof(void *)) {
    throw std::runtime_error(
        fmt::format("Stride {} is too small, expected at least {}", Stride,
                    sizeof(void *)));
  }
  if (Stride * Count > RegionSize) {
    throw std::runtime_error(
        fmt::format("Region size of {} is too small for Stride {} and Count {}",
                    RegionSize, Stride, Count));
  }

  for (int I = Count - 1; I > 0; --I) {
    reinterpret_cast<char *&>(Region[Stride * I]) = &Region[Stride * (I - 1)];
  }
  reinterpret_cast<char *&>(Region[0]) = &Region[Stride * (Count - 1)];
}

static auto measureFor(char *Region, int RegionSize, int Stride, int Count) {
  preparePointerChain(Region, RegionSize, Stride, Count);
  char *Current = &Region[Stride * (Count - 1)];

  // clang-format off
#define STEP_1 Current = *reinterpret_cast<char **>(Current);
#define STEP_10 STEP_1 STEP_1 STEP_1 STEP_1 STEP_1 STEP_1 STEP_1 STEP_1 STEP_1 STEP_1
#define STEP_100 STEP_10 STEP_10 STEP_10 STEP_10 STEP_10 STEP_10 STEP_10 STEP_10 STEP_10 STEP_10
#define STEP_1000 STEP_100 STEP_100 STEP_100 STEP_100 STEP_100 STEP_100 STEP_100 STEP_100 STEP_100 STEP_100
  // clang-format on

  STEP_10; // warm up
  auto Start = now();
  STEP_1000;
  auto Finish = now();
  auto Duration = Finish - Start;
  return Duration;
}

static int getAssocFor(char *Region, int RegionSize, int Stride) {
  auto OldTime = measureFor(Region, RegionSize, Stride, 1);
  for (int Assoc = 2; Assoc <= 64; ++Assoc) {
    auto Time = measureFor(Region, RegionSize, Stride, Assoc);
    if (Time.count() / (double)OldTime.count() > JumpFactor)
      return Assoc - 1;
    OldTime = Time;
  }
  throw std::runtime_error(
      fmt::format("Failed to get Assoc for Stride {}", Stride));
}

struct SizeAndAssoc {
  int Size;
  int Assoc;
};

static SizeAndAssoc runRobustSizeAndAssoc() {
  int OldAssoc;
  for (int WaySize = MaxWaySize; WaySize >= 16; WaySize /= 2) {
    int Assoc = getAssocFor(TestRegion, sizeof(TestRegion), WaySize);
    if (WaySize != MaxWaySize && Assoc == 2 * OldAssoc) {
      return SizeAndAssoc{
          .Size = 2 * WaySize * OldAssoc,
          .Assoc = OldAssoc,
      };
    }
    OldAssoc = Assoc;
  }
  throw std::runtime_error(fmt::format("Failed to get Size and Assoc"));
}

static void runMeasureTool() {
  if (int RealPageSize = getpagesize(); RealPageSize != PageSizeBytes) {
    throw std::runtime_error(
        fmt::format("Oops, expected page size {}, but actually it's {}\n",
                    PageSizeBytes, RealPageSize));
  }
  auto [Size, Assoc] = runRobustSizeAndAssoc();
  std::cerr << fmt::format("Size = {}\n", Size);
  std::cerr << fmt::format("Assoc = {}\n", Assoc);
}

int main() {
  try {
    runMeasureTool();
  } catch (std::runtime_error &Error) {
    std::cerr << "Fatal: " << Error.what() << std::endl;
    return -1;
  }
  return 0;
}
