#include "fmt/chrono.h"
#include "fmt/format.h"
#include <array>
#include <chrono>
#include <iostream>

static constexpr int MaxTestRegionSizeBytes = 128 * 1024 * 1024;
static constexpr int MeasureIters = 256 * 1024 * 1024;
static constexpr int MaxWaySize = 1024 * 128;
static constexpr int MinBlockSize = 16;
static constexpr int MaxBlockSize = 1024;
static constexpr double MissFactor = 1.05;
static constexpr int MaxAssoc = 32;
static constexpr int WarmUpCount = 8 * 1024;
static constexpr int WarmUpStride = 64;

alignas(1024 * 1024) char TestRegion1[MaxTestRegionSizeBytes];
alignas(1024 * 1024) char TestRegion2[MaxTestRegionSizeBytes];

bool DebugDump = false;

intptr_t Dummy;

using Clock = std::chrono::high_resolution_clock;
using Duration = Clock::duration;

static auto now() { return Clock::now(); }

void warmUp() {
  char *Current = TestRegion2;
  for (int I = 0; I < 8 * WarmUpCount; ++I) {
    Current = *reinterpret_cast<char **>(Current);
  }
  Dummy += (intptr_t)Current;
}

static Duration measureForPointerChain(char *Initial) {
  warmUp();

  char *Current = Initial;
  // clang-format off
#define STEP_1 Current = *reinterpret_cast<char **>(Current);
#define STEP_10 STEP_1 STEP_1 STEP_1 STEP_1 STEP_1 STEP_1 STEP_1 STEP_1 STEP_1 STEP_1
#define STEP_100 STEP_10 STEP_10 STEP_10 STEP_10 STEP_10 STEP_10 STEP_10 STEP_10 STEP_10 STEP_10
#define STEP_1000 STEP_100 STEP_100 STEP_100 STEP_100 STEP_100 STEP_100 STEP_100 STEP_100 STEP_100 STEP_100
#define STEP_10000 STEP_1000 STEP_1000 STEP_1000 STEP_1000 STEP_1000 STEP_1000 STEP_1000 STEP_1000 STEP_1000 STEP_1000
  // clang-format on

  STEP_100; // warm up
  auto Start = now();
  STEP_10000;
  STEP_10000;
  STEP_10000;
  STEP_10000;
  auto Finish = now();

  // auto Start = now();
  // for (int I = 0; I < MeasureIters; ++I) {
  //   Current = *reinterpret_cast<char **>(Current);
  // }
  // auto Finish = now();
  Dummy += (intptr_t)Current;
  auto Duration = Finish - Start;
  return Duration;
}

static void setPointer(char &Location, char *Value) {
  reinterpret_cast<char *&>(Location) = Value;
}

static void chainPointersForArithmeticSeq(char *First, int Stride, int Count) {
  for (int I = Count - 1; I > 0; --I) {
    setPointer(First[Stride * I], &First[Stride * (I - 1)]);
  }
}

static void preparePointerChainForArithmeticSeq(char *Region, int RegionSize,
                                                int Stride, int Count) {
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

  chainPointersForArithmeticSeq(Region, Stride, Count);
  setPointer(Region[0], &Region[Stride * (Count - 1)]);
}

static auto measureForArithmeticSeq(int Stride, int Count) {
  preparePointerChainForArithmeticSeq(TestRegion1, sizeof(TestRegion1), Stride,
                                      Count);
  return measureForPointerChain(&TestRegion1[Stride * (Count - 1)]);
}

struct SizeAndAssoc {
  int Size;
  int Assoc;
};

static SizeAndAssoc runRobustSizeAndAssoc() {
  auto GetAssocFor = [](int Stride) {
    std::array<Duration, MaxAssoc + 2> RealMeasurements, HitMeasurements;
    for (int Assoc = 1; Assoc <= MaxAssoc + 1; ++Assoc) {
      RealMeasurements[Assoc] = measureForArithmeticSeq(Stride, Assoc);
      HitMeasurements[Assoc] = measureForArithmeticSeq(256, Assoc);
      if (DebugDump) {
        std::cerr << fmt::format(
            "Stride = {} Assoc = {}: real/hit = {}/{} = {}\n", Stride, Assoc,
            RealMeasurements[Assoc], HitMeasurements[Assoc],
            RealMeasurements[Assoc].count() /
                (double)HitMeasurements[Assoc].count());
      }
    }
    for (int Assoc = 2; Assoc <= MaxAssoc + 1; ++Assoc) {
      double Ratio = RealMeasurements[Assoc].count() /
                     (double)HitMeasurements[Assoc].count();
      if (Ratio > MissFactor)
        return Assoc - 1;
    }
    throw std::runtime_error(
        fmt::format("Failed to get Assoc for Stride {}", Stride));
  };

  int OldAssoc;
  for (int WaySize = MaxWaySize; WaySize >= 16; WaySize /= 2) {
    int Assoc = GetAssocFor(WaySize);
    if (DebugDump) {
      std::cerr << fmt::format("WaySize = {} Assoc = {}\n", WaySize, Assoc);
    }
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

static int runRobustBlockSize(int Size, int Assoc) {
  int WaySize = Size / Assoc;
  Duration OldTime;
  for (int CurrentBlockSize = MinBlockSize;
       CurrentBlockSize <= 2 * MaxBlockSize; CurrentBlockSize *= 2) {
    chainPointersForArithmeticSeq(TestRegion1, WaySize, Assoc / 2);
    char *Next = TestRegion1 + WaySize * (Assoc / 2) + CurrentBlockSize;
    setPointer(*Next, &TestRegion1[WaySize * (Assoc / 2 - 1)]);
    chainPointersForArithmeticSeq(Next, WaySize, Assoc / 2 + 1);
    setPointer(TestRegion1[0], Next + WaySize * (Assoc / 2));
    auto Time = measureForPointerChain(TestRegion1);
    if (CurrentBlockSize > MinBlockSize &&
        OldTime.count() / (double)Time.count() > MissFactor) {
      return CurrentBlockSize;
    }
    OldTime = Time;
  }
  throw std::runtime_error(fmt::format("Failed to measure BlockSize"));
}

static void runMeasureTool() {
  chainPointersForArithmeticSeq(TestRegion2, WarmUpStride, WarmUpCount);
  setPointer(*TestRegion2, &TestRegion2[WarmUpStride * (WarmUpCount - 1)]);
  auto [Size, Assoc] = runRobustSizeAndAssoc();
  std::cerr << fmt::format("Size = {}\n", Size);
  std::cerr << fmt::format("Assoc = {}\n", Assoc);
  int BlockSize = runRobustBlockSize(Size, Assoc);
  std::cerr << fmt::format("BlockSize = {}\n", BlockSize);
}

int main() {
  std::cerr << "std::chrono::high_resolution_clock::period = "
            << Clock::period::num << '/' << Clock::period::den << '\n';
  if (char *DebugDumpValue = std::getenv("DEBUG_DUMP");
      DebugDumpValue && std::string(DebugDumpValue) == "1") {
    DebugDump = true;
  }
  try {
    runMeasureTool();
  } catch (std::runtime_error &Error) {
    std::cerr << "Fatal: " << Error.what() << std::endl;
    return -1;
  }
  return 0;
}
