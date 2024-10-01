#include "fmt/chrono.h"
#include "fmt/format.h"
#include <array>
#include <chrono>
#include <iostream>
#include <unistd.h>

static constexpr int PageSizeBytes = 4096;
static constexpr int MaxTestRegionSizeBytes = 128 * 1024 * 1024;
static constexpr int MeasureIters = 1024 * 1024;
static constexpr int MaxWaySize = 1024 * 1024;
static constexpr int MinBlockSize = 16;
static constexpr int MaxBlockSize = 1024;
static constexpr double MissFactor = 2.0;
static constexpr int MaxAssoc = 64;

alignas(1024 * 1024) char TestRegion[MaxTestRegionSizeBytes];

bool DebugDump = false;

size_t Dummy;

static auto now() { return std::chrono::high_resolution_clock::now(); }

using Duration = std::chrono::high_resolution_clock::duration;

static auto measureForPointerChain(char *Initial) {
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
  Dummy += (size_t)Current;
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
  preparePointerChainForArithmeticSeq(TestRegion, sizeof(TestRegion), Stride,
                                      Count);
  return measureForPointerChain(&TestRegion[Stride * (Count - 1)]);
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
      if (DebugDump) {
        std::cerr << fmt::format("Stride = {} Assoc = {}: {}\n", Stride, Assoc,
                                 RealMeasurements[Assoc]);
      }
      HitMeasurements[Assoc] = measureForArithmeticSeq(256, Assoc);
      if (DebugDump) {
        std::cerr << fmt::format("Stride = {} Assoc = {}: fakeTime = {}\n",
                                 Stride, Assoc, HitMeasurements[Assoc]);
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
    chainPointersForArithmeticSeq(TestRegion, WaySize, Assoc / 2);
    char *Next = TestRegion + WaySize * (Assoc / 2) + CurrentBlockSize;
    setPointer(*Next, &TestRegion[WaySize * (Assoc / 2 - 1)]);
    chainPointersForArithmeticSeq(Next, WaySize, Assoc / 2 + 1);
    setPointer(TestRegion[0], Next + WaySize * (Assoc / 2));
    auto Time = measureForPointerChain(TestRegion);
    if (CurrentBlockSize > MinBlockSize &&
        OldTime.count() / (double)Time.count() > MissFactor) {
      return CurrentBlockSize;
    }
    OldTime = Time;
  }
  throw std::runtime_error(fmt::format("Failed to measure BlockSize"));
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
  int BlockSize = runRobustBlockSize(Size, Assoc);
  std::cerr << fmt::format("BlockSize = {}\n", BlockSize);
}

int main() {
  // struct sched_param SchedParam;
  // if (int ret = sched_getparam(0, &SchedParam); ret == -1) {
  //   std::cerr << fmt::format("sched_getparam failed\n");
  //   return -1;
  // }
  // SchedParam.sched_priority = sched_get_priority_max(SCHED_FIFO);
  // if (int ret = sched_setscheduler(0, SCHED_FIFO, &SchedParam); ret == -1) {
  //   std::cerr << fmt::format("sched_setscheduler failed\n");
  //   return -1;
  // }
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
