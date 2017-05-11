// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fcntl.h>
#include <thread>

#include "helper/platform_device_helper.h"
#include "magenta/magenta_platform_ioctl.h"
#include "magma.h"
#include "magma_util/macros.h"
#include "gtest/gtest.h"

namespace {

class TestBase {
public:
    TestBase() { fd_ = open("/dev/class/display/000", O_RDONLY); }

    int fd() { return fd_; }

    bool is_intel_gen()
    {
        uint64_t device_id;
        if (magma_query(fd(), MAGMA_QUERY_DEVICE_ID, &device_id) != MAGMA_STATUS_OK)
            device_id = 0;
        return TestPlatformDevice::is_intel_gen(device_id);
    }

    ~TestBase() { close(fd_); }

private:
    int fd_;
};

class TestConnection : public TestBase {
public:
    TestConnection() { connection_ = magma_open(fd(), MAGMA_CAPABILITY_RENDERING); }

    ~TestConnection()
    {
        if (connection_)
            magma_close(connection_);
    }

    int32_t Test()
    {
        DASSERT(connection_);

        uint32_t context_id;
        magma_create_context(connection_, &context_id);

        int32_t result = magma_get_error(connection_);
        if (result != 0)
            return DRET(result);

        uint64_t size;
        magma_buffer_t batch_buffer, command_buffer;

        result = magma_alloc(connection_, PAGE_SIZE, &size, &batch_buffer);
        if (result != 0)
            return DRET(result);

        result = magma_alloc(connection_, PAGE_SIZE, &size, &command_buffer);
        if (result != 0)
            return DRET(result);

        EXPECT_TRUE(InitBatchBuffer(batch_buffer, size));
        EXPECT_TRUE(InitCommandBuffer(command_buffer, batch_buffer, size));

        magma_submit_command_buffer(connection_, command_buffer, context_id);
        magma_wait_rendering(connection_, batch_buffer);

        magma_destroy_context(connection_, context_id);
        magma_free(connection_, batch_buffer);
        magma_free(connection_, command_buffer);

        result = magma_get_error(connection_);
        return DRET(result);
    }

    bool InitBatchBuffer(magma_buffer_t buffer, uint64_t size)
    {
        if (!is_intel_gen())
            return DRETF(false, "not an intel gen9 device");

        void* vaddr;
        if (magma_map(connection_, buffer, &vaddr) != 0)
            return DRETF(false, "couldn't map batch buffer");

        memset(vaddr, 0, size);

        // Intel end-of-batch
        *reinterpret_cast<uint32_t*>(vaddr) = 0xA << 23;

        EXPECT_EQ(magma_unmap(connection_, buffer), 0);

        return true;
    }

    bool InitCommandBuffer(magma_buffer_t buffer, magma_buffer_t batch_buffer,
                           uint64_t batch_buffer_length)
    {
        void* vaddr;
        if (magma_map(connection_, buffer, &vaddr) != 0)
            return DRETF(false, "couldn't map command buffer");

        auto command_buffer = reinterpret_cast<struct magma_system_command_buffer*>(vaddr);
        command_buffer->batch_buffer_resource_index = 0;
        command_buffer->batch_start_offset = 0;
        command_buffer->num_resources = 1;

        auto exec_resource =
            reinterpret_cast<struct magma_system_exec_resource*>(command_buffer + 1);
        exec_resource->buffer_id = magma_get_buffer_id(batch_buffer);
        exec_resource->num_relocations = 0;
        exec_resource->offset = 0;
        exec_resource->length = batch_buffer_length;

        EXPECT_EQ(magma_unmap(connection_, buffer), 0);

        return true;
    }

private:
    magma_connection_t* connection_;
};

constexpr uint32_t kMaxCount = 100;
constexpr uint32_t kRestartCount = kMaxCount / 10;

static std::atomic_uint complete_count;

static void looper_thread_entry()
{
    std::unique_ptr<TestConnection> test(new TestConnection());
    while (complete_count < kMaxCount) {
        int32_t result = test->Test();
        if (result == 0) {
            complete_count++;
        } else {
            // Wait rendering can't pass back a proper error yet
            EXPECT_TRUE(result == MAGMA_STATUS_CONNECTION_LOST ||
                        result == MAGMA_STATUS_INTERNAL_ERROR);
            test.reset(new TestConnection());
        }
    }
}

static void test_shutdown(uint32_t iters)
{
    for (uint32_t i = 0; i < iters; i++) {
        complete_count = 0;

        TestBase test_base;
        ASSERT_TRUE(test_base.is_intel_gen());

        std::thread looper(looper_thread_entry);
        std::thread looper2(looper_thread_entry);

        uint32_t count = kRestartCount;
        while (complete_count < kMaxCount) {
            if (complete_count > count) {
                // Should replace this with a request to devmgr to restart the driver
                EXPECT_EQ(
                    mxio_ioctl(test_base.fd(), IOCTL_MAGMA_TEST_RESTART, nullptr, 0, nullptr, 0),
                    0);
                count += kRestartCount;
            }
            std::this_thread::yield();
        }

        looper.join();
        looper2.join();
    }
}

} // namespace

TEST(Shutdown, Test) { test_shutdown(1); }

TEST(Shutdown, Stress) { test_shutdown(1000); }
