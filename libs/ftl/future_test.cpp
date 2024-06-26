/*
 * Copyright 2020 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <ftl/future.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <future>
#include <string>
#include <thread>
#include <vector>

namespace android::test {

// Keep in sync with example usage in header file.
TEST(Future, Example) {
  {
    auto future = ftl::defer([](int x) { return x + 1; }, 99);
    EXPECT_EQ(future.get(), 100);
  }
  {
    auto future = ftl::yield(42);
    EXPECT_EQ(future.get(), 42);
  }
  {
    auto ptr = std::make_unique<char>('!');
    auto future = ftl::yield(std::move(ptr));
    EXPECT_EQ(*future.get(), '!');
  }
  {
    auto future = ftl::yield(123);
    ftl::Future<char> futures[] = {ftl::yield('a'), ftl::yield('b')};

    ftl::Future<char> chain = ftl::Future(std::move(future))
                                  .then([](int x) { return static_cast<size_t>(x % 2); })
                                  .then([&futures](size_t i) { return std::move(futures[i]); });

    EXPECT_EQ(chain.get(), 'b');
  }
}

namespace {

using ByteVector = std::vector<uint8_t>;

ByteVector decrement(ByteVector bytes) {
  std::transform(bytes.begin(), bytes.end(), bytes.begin(), [](auto b) { return b - 1; });
  return bytes;
}

}  // namespace

TEST(Future, Chain) {
  std::packaged_task<const char*()> fetch_string([] { return "ifmmp-"; });

  std::packaged_task<ByteVector(std::string)> append_string([](std::string str) {
    str += "!xpsme";
    return ByteVector{str.begin(), str.end()};
  });

  std::packaged_task<ftl::Future<ByteVector>(ByteVector)> decrement_bytes(
      [](ByteVector bytes) { return ftl::defer(decrement, std::move(bytes)); });

  auto fetch = fetch_string.get_future();
  std::thread fetch_thread(std::move(fetch_string));

  std::thread append_thread, decrement_thread;

  EXPECT_EQ(
      "hello, world",
      ftl::Future(std::move(fetch))
          .then([](const char* str) { return std::string(str); })
          .then([&](std::string str) {
            auto append = append_string.get_future();
            append_thread = std::thread(std::move(append_string), std::move(str));
            return append;
          })
          .then([&](ByteVector bytes) {
            auto decrement = decrement_bytes.get_future();
            decrement_thread = std::thread(std::move(decrement_bytes), std::move(bytes));
            return decrement;
          })
          .then([](ftl::Future<ByteVector> bytes) { return bytes; })
          .then([](const ByteVector& bytes) { return std::string(bytes.begin(), bytes.end()); })
          .get());

  fetch_thread.join();
  append_thread.join();
  decrement_thread.join();
}

TEST(Future, WaitFor) {
  using namespace std::chrono_literals;
  {
    auto future = ftl::yield(42);
    // Check that we can wait_for multiple times without invalidating the future
    EXPECT_EQ(future.wait_for(1s), std::future_status::ready);
    EXPECT_EQ(future.wait_for(1s), std::future_status::ready);
    EXPECT_EQ(future.get(), 42);
  }

  {
    std::condition_variable cv;
    std::mutex m;
    bool ready = false;

    std::packaged_task<int32_t()> get_int([&] {
      std::unique_lock lk(m);
      cv.wait(lk, [&] { return ready; });
      return 24;
    });

    auto get_future = ftl::Future(get_int.get_future());
    std::thread get_thread(std::move(get_int));

    EXPECT_EQ(get_future.wait_for(0s), std::future_status::timeout);
    {
      std::unique_lock lk(m);
      ready = true;
    }
    cv.notify_one();

    EXPECT_EQ(get_future.wait_for(1s), std::future_status::ready);
    EXPECT_EQ(get_future.get(), 24);

    get_thread.join();
  }
}

}  // namespace android::test
