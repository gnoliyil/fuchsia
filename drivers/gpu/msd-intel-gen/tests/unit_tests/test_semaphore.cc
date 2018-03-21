// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "magma_util/dlog.h"
#include "msd_intel_semaphore.h"
#include "gtest/gtest.h"
#include <thread>

namespace {

class TestMsdIntelSemaphore {
public:
    static void Test()
    {
        std::shared_ptr<magma::PlatformSemaphore> semaphore(magma::PlatformSemaphore::Create());
        ASSERT_NE(semaphore, nullptr);

        auto abi_semaphore = std::make_shared<MsdIntelAbiSemaphore>(semaphore);

        EXPECT_EQ(abi_semaphore->ptr()->id(), semaphore->id());
        EXPECT_EQ(2, semaphore.use_count());

        // Verify return before timeout
        std::thread thread([abi_semaphore] {
            DLOG("Waiting for semaphore");
            EXPECT_TRUE(abi_semaphore->ptr()->Wait(100));
            DLOG("Semaphore wait returned");
        });
        semaphore->Signal();
        thread.join();

        abi_semaphore.reset();

        EXPECT_EQ(1, semaphore.use_count());
    }
};
}

// TODO(MA-433): deflake
TEST(MsdIntelSemaphore, DISABLED_Test) { TestMsdIntelSemaphore::Test(); }
