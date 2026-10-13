#include <mbgl/test/util.hpp>
#include <mbgl/renderer/android_memory_sample.hpp>

using namespace mbgl;

TEST(AndroidMemorySample, CountsResidentAndSwappedPagesNotVirtualAddressSpace) {
    const auto memory = android_memory::residentAndSwapKiB(
        "Name:\tterrain\nVmSize:\t99999999 kB\nVmRSS:\t819200 kB\n"
        "RssAnon:\t600000 kB\nVmSwap:\t409600 kB\n");
    ASSERT_TRUE(memory);
    EXPECT_EQ(*memory, 1228800u);
}

TEST(AndroidMemorySample, AcceptsZeroSwapAndKernelWhitespace) {
    EXPECT_EQ(android_memory::residentAndSwapKiB("VmSwap: 0 kB\nVmRSS:\t 1024 kB\r\n"), 1024u);
}

TEST(AndroidMemorySample, AcceptsReorderedFieldsAndNoFinalNewline) {
    EXPECT_EQ(android_memory::residentAndSwapKiB(
                  "VmSwap:\t2048 kB\nThreads:\t9\nVmRSS:\t1024 kB"),
              3072u);
    EXPECT_EQ(android_memory::residentAndSwapKiB("VmRSS: 0 kB\nVmSwap: 0 kB"), 0u);
}

TEST(AndroidMemorySample, IgnoresSimilarFieldNamesAndUnrelatedCounters) {
    EXPECT_EQ(android_memory::residentAndSwapKiB(
                  "VmRSSPeak: 18446744073709551615 kB\nRssAnon: 999999 kB\n"
                  "VmSwapPeak: 999999 kB\nVmRSS: 2048 kB\nVmSwap: 1024 kB\n"),
              3072u);
    EXPECT_FALSE(android_memory::residentAndSwapKiB("VmRSSPeak: 2048 kB\nVmSwap: 1024 kB\n"));
}

TEST(AndroidMemorySample, AcceptsLargestRepresentableSumWithoutOverflow) {
    constexpr auto maximum = std::numeric_limits<uint64_t>::max();
    EXPECT_EQ(android_memory::residentAndSwapKiB(
                  "VmRSS: 18446744073709551614 kB\nVmSwap: 1 kB\n"),
              maximum);
    EXPECT_EQ(android_memory::residentAndSwapKiB(
                  "VmRSS: 0 kB\nVmSwap: 18446744073709551615 kB\n"),
              maximum);
}

TEST(AndroidMemorySample, RejectsUnavailableOrMalformedMeasurements) {
    for (const auto status : {"", "VmRSS: 1024 kB\n", "VmSwap: 0 kB\n",
                              "VmRSS: -1 kB\nVmSwap: 0 kB\n",
                              "VmRSS: 1024 MB\nVmSwap: 0 kB\n",
                              "VmRSS: 1.5 kB\nVmSwap: 0 kB\n",
                              "VmRSS: +1024 kB\nVmSwap: 0 kB\n",
                              "VmRSS: 1024\nVmSwap: 0 kB\n",
                              "VmRSS: 1024 kB garbage\nVmSwap: 0 kB\n",
                              "VmRSS: \t\nVmSwap: 0 kB\n",
                              "VmRSS: 1024 kB\nVmSwap: nope kB\n",
                              "VmRSS: 1 kB\nVmRSS: 2 kB\nVmSwap: 0 kB\n",
                              "VmRSS: 1 kB\nVmSwap: 0 kB\nVmSwap: 1 kB\n",
                              "VmRSS: 18446744073709551616 kB\nVmSwap: 0 kB\n",
                              "VmRSS: 18446744073709551615 kB\nVmSwap: 1 kB\n"}) {
        EXPECT_FALSE(android_memory::residentAndSwapKiB(status)) << status;
    }
}
