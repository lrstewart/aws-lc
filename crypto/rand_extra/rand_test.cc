/* Copyright (c) 2018, Google Inc.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE. */

#include <openssl/rand.h>

#include <stdio.h>

#include <gtest/gtest.h>

#include <openssl/span.h>

#include "../fipsmodule/rand/fork_detect.h"
#include "../fipsmodule/rand/internal.h"
#include "../test/abi_test.h"
#include "../test/test_util.h"

#if defined(OPENSSL_THREADS)
#include <array>
#include <thread>
#include <vector>
#endif

#if !defined(OPENSSL_WINDOWS)
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

static void maybe_disable_some_fork_detect_mechanisms(void) {
#if defined(OPENSSL_LINUX)
  if (getenv("BORINGSSL_IGNORE_MADV_WIPEONFORK")) {
    CRYPTO_fork_detect_ignore_madv_wipeonfork_for_testing();
  }
#endif
}


// These tests are, strictly speaking, flaky, but we use large enough buffers
// that the probability of failing when we should pass is negligible.

TEST(RandTest, NotObviouslyBroken) {
  static const uint8_t kZeros[256] = {0};

  maybe_disable_some_fork_detect_mechanisms();

  uint8_t buf1[256], buf2[256];
  RAND_bytes(buf1, sizeof(buf1));
  RAND_bytes(buf2, sizeof(buf2));

  EXPECT_NE(Bytes(buf1), Bytes(buf2));
  EXPECT_NE(Bytes(buf1), Bytes(kZeros));
  EXPECT_NE(Bytes(buf2), Bytes(kZeros));
}

#if !defined(OPENSSL_WINDOWS) && !defined(OPENSSL_IOS) && \
    !defined(BORINGSSL_UNSAFE_DETERMINISTIC_MODE)
static bool ForkAndRand(bssl::Span<uint8_t> out, bool fork_unsafe_buffering) {
  int pipefds[2];
  if (pipe(pipefds) < 0) {
    perror("pipe");
    return false;
  }

  // This is a multi-threaded process, but GTest does not run tests concurrently
  // and there currently are no threads, so this should be safe.
  pid_t child = fork();
  if (child < 0) {
    perror("fork");
    close(pipefds[0]);
    close(pipefds[1]);
    return false;
  }

  if (child == 0) {
    // This is the child. Generate entropy and write it to the parent.
    close(pipefds[0]);
    if (fork_unsafe_buffering) {
      RAND_enable_fork_unsafe_buffering(-1);
    }
    RAND_bytes(out.data(), out.size());
    while (!out.empty()) {
      ssize_t ret = write(pipefds[1], out.data(), out.size());
      if (ret < 0) {
        if (errno == EINTR) {
          continue;
        }
        perror("write");
        _exit(1);
      }
      out = out.subspan(static_cast<size_t>(ret));
    }
    _exit(0);
  }

  // This is the parent. Read the entropy from the child.
  close(pipefds[1]);
  while (!out.empty()) {
    ssize_t ret = read(pipefds[0], out.data(), out.size());
    if (ret <= 0) {
      if (ret == 0) {
        fprintf(stderr, "Unexpected EOF from child.\n");
      } else {
        if (errno == EINTR) {
          continue;
        }
        perror("read");
      }
      close(pipefds[0]);
      return false;
    }
    out = out.subspan(static_cast<size_t>(ret));
  }
  close(pipefds[0]);

  // Wait for the child to exit.
  int status;
  if (waitpid(child, &status, 0) < 0) {
    perror("waitpid");
    return false;
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    fprintf(stderr, "Child did not exit cleanly.\n");
    return false;
  }

  return true;
}

TEST(RandTest, Fork) {
  static const uint8_t kZeros[16] = {0};

  maybe_disable_some_fork_detect_mechanisms();

  // Draw a little entropy to initialize any internal PRNG buffering.
  uint8_t byte;
  RAND_bytes(&byte, 1);

  // Draw entropy in two child processes and the parent process. This test
  // intentionally uses smaller buffers than the others, to minimize the chance
  // of sneaking by with a large enough buffer that we've since reseeded from
  // the OS.
  //
  // All child processes should have different PRNGs, including the ones that
  // disavow fork-safety. Although they are produced by fork, they themselves do
  // not fork after that call.
  uint8_t bufs[5][16];
  ASSERT_TRUE(ForkAndRand(bufs[0], /*fork_unsafe_buffering=*/false));
  ASSERT_TRUE(ForkAndRand(bufs[1], /*fork_unsafe_buffering=*/false));
  ASSERT_TRUE(ForkAndRand(bufs[2], /*fork_unsafe_buffering=*/true));
  ASSERT_TRUE(ForkAndRand(bufs[3], /*fork_unsafe_buffering=*/true));
  RAND_bytes(bufs[4], sizeof(bufs[4]));

  // All should be different and non-zero.
  for (const auto &buf : bufs) {
    EXPECT_NE(Bytes(buf), Bytes(kZeros));
  }
  for (size_t i = 0; i < OPENSSL_ARRAY_SIZE(bufs); i++) {
    for (size_t j = 0; j < i; j++) {
      EXPECT_NE(Bytes(bufs[i]), Bytes(bufs[j]))
          << "buffers " << i << " and " << j << " matched";
    }
  }
}
#endif  // !OPENSSL_WINDOWS && !OPENSSL_IOS &&
        // !BORINGSSL_UNSAFE_DETERMINISTIC_MODE

#if defined(OPENSSL_THREADS)
static void RunConcurrentRands(size_t num_threads) {
  static const uint8_t kZeros[256] = {0};

  std::vector<std::array<uint8_t, 256>> bufs(num_threads);
  std::vector<std::thread> threads(num_threads);

  for (size_t i = 0; i < num_threads; i++) {
    threads[i] =
        std::thread([i, &bufs] { RAND_bytes(bufs[i].data(), bufs[i].size()); });
  }
  for (size_t i = 0; i < num_threads; i++) {
    threads[i].join();
  }

  for (size_t i = 0; i < num_threads; i++) {
    EXPECT_NE(Bytes(bufs[i]), Bytes(kZeros));
    for (size_t j = i + 1; j < num_threads; j++) {
      EXPECT_NE(Bytes(bufs[i]), Bytes(bufs[j]));
    }
  }
}

// Test that threads may concurrently draw entropy without tripping TSan.
TEST(RandTest, Threads) {
  constexpr size_t kFewerThreads = 10;
  constexpr size_t kMoreThreads = 20;

  maybe_disable_some_fork_detect_mechanisms();

  // Draw entropy in parallel.
  RunConcurrentRands(kFewerThreads);
  // Draw entropy in parallel with higher concurrency than the previous maximum.
  RunConcurrentRands(kMoreThreads);
  // Draw entropy in parallel with lower concurrency than the previous maximum.
  RunConcurrentRands(kFewerThreads);
}
#endif  // OPENSSL_THREADS

#if defined(OPENSSL_X86_64) && defined(SUPPORTS_ABI_TEST)
TEST(RandTest, RdrandABI) {
  if (!have_rdrand()) {
    fprintf(stderr, "rdrand not supported. Skipping.\n");
    return;
  }

  uint8_t buf[32];
  CHECK_ABI_SEH(CRYPTO_rdrand, buf);
  CHECK_ABI_SEH(CRYPTO_rdrand_multiple8_buf, nullptr, 0);
  CHECK_ABI_SEH(CRYPTO_rdrand_multiple8_buf, buf, 8);
  CHECK_ABI_SEH(CRYPTO_rdrand_multiple8_buf, buf, 16);
  CHECK_ABI_SEH(CRYPTO_rdrand_multiple8_buf, buf, 24);
  CHECK_ABI_SEH(CRYPTO_rdrand_multiple8_buf, buf, 32);
}
#endif  // OPENSSL_X86_64 && SUPPORTS_ABI_TEST

#if defined(AWSLC_FIPS) && defined(FIPS_ENTROPY_SOURCE_PASSIVE)
TEST(RandTest, PassiveEntropyLoad) {
  uint8_t out_entropy[CTR_DRBG_ENTROPY_LEN] = {0};
  uint8_t entropy[PASSIVE_ENTROPY_LOAD_LENGTH] = {
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
  };
  uint8_t expected_out_entropy[CTR_DRBG_ENTROPY_LEN] = {
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
  };

  RAND_load_entropy(out_entropy, entropy);

  EXPECT_EQ(Bytes(out_entropy), Bytes(expected_out_entropy));
}

TEST(RandTest, PassiveEntropyDepletedObviouslyNotBroken) {

  static const uint8_t kZeros[CTR_DRBG_ENTROPY_LEN] = {0};
  uint8_t buf1[CTR_DRBG_ENTROPY_LEN] = {0};
  uint8_t buf2[CTR_DRBG_ENTROPY_LEN] = {0};
  int out_want_additional_input_false_default = 0;
  int out_want_additional_input_true_default = 1;

  RAND_module_entropy_depleted(buf1, &out_want_additional_input_false_default);
  RAND_module_entropy_depleted(buf2, &out_want_additional_input_true_default);
  EXPECT_TRUE(out_want_additional_input_false_default == 0 || out_want_additional_input_false_default == 1);
  EXPECT_TRUE(out_want_additional_input_true_default == 0 || out_want_additional_input_true_default == 1);

// |have_rdrand| inlines the cpu capability vector ending up with an undefined
// reference because the variable has internal linkage in the shared build. So,
// we can only validate the correct value is set on the static build type.
#if !defined(BORINGSSL_SHARED_LIBRARY)
  int want_additional_input_expect = 0;
  if (have_rdrand()) {
    want_additional_input_expect = 1;
  }
  EXPECT_EQ(out_want_additional_input_false_default, want_additional_input_expect);
  EXPECT_EQ(out_want_additional_input_true_default, want_additional_input_expect);
#endif

  EXPECT_NE(Bytes(buf1), Bytes(buf2));
  EXPECT_NE(Bytes(buf1), Bytes(kZeros));
  EXPECT_NE(Bytes(buf2), Bytes(kZeros));
}
#endif
