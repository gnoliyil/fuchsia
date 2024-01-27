// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aml-gpio.h"

#include <lib/ddk/platform-defs.h>

#include <fbl/alloc_checker.h>
#include <mock-mmio-reg/mock-mmio-reg.h>

#include "a113-blocks.h"
#include "lib/device-protocol/pdev-fidl.h"
#include "s905d2-blocks.h"

namespace {

constexpr size_t kGpioRegSize = 0x100;
constexpr size_t kInterruptRegSize = 0x30;
constexpr size_t kInterruptRegOffset = 0x3c00;

// Fake functions
zx_status_t mmio_fn(void *ctx, uint32_t index, pdev_mmio_t *out_mmio) { return ZX_OK; }
zx_status_t interrupt_fn(void *ctx, uint32_t index, uint32_t flags, zx_handle_t *out_irq) {
  return ZX_OK;
}
zx_status_t bti_fn(void *ctx, uint32_t index, zx_handle_t *out_bti) { return ZX_OK; }
zx_status_t smc_fn(void *ctx, uint32_t index, zx_handle_t *out_smc) { return ZX_OK; }
zx_status_t device_info_fn(void *ctx, pdev_device_info_t *out_info) { return ZX_OK; }
zx_status_t board_info_fn(void *ctx, pdev_board_info_t *out_info) { return ZX_OK; }

pdev_protocol_ops_t fake_ops{.get_mmio = &mmio_fn,
                             .get_interrupt = &interrupt_fn,
                             .get_bti = &bti_fn,
                             .get_smc = &smc_fn,
                             .get_device_info = &device_info_fn,
                             .get_board_info = &board_info_fn};
pdev_protocol_t fake_proto{.ops = &fake_ops, .ctx = nullptr};

}  // namespace

namespace gpio {

class FakeAmlGpio : public AmlGpio {
 public:
  static FakeAmlGpio *Create(pdev_device_info info, ddk_mock::MockMmioRegRegion *mock_mmio_gpio,
                             ddk_mock::MockMmioRegRegion *mock_mmio_gpio_a0,
                             ddk_mock::MockMmioRegRegion *mock_mmio_interrupt) {
    const AmlGpioBlock *gpio_blocks;
    const AmlGpioInterrupt *gpio_interrupt;
    size_t block_count;

    switch (info.pid) {
      case PDEV_PID_AMLOGIC_A113:
        gpio_blocks = a113_gpio_blocks;
        block_count = std::size(a113_gpio_blocks);
        gpio_interrupt = &a113_interrupt_block;
        break;
      case PDEV_PID_AMLOGIC_S905D2:
      case PDEV_PID_AMLOGIC_T931:
        // S905D2 and T931 are identical.
        gpio_blocks = s905d2_gpio_blocks;
        block_count = std::size(s905d2_gpio_blocks);
        gpio_interrupt = &s905d2_interrupt_block;
        break;
      default:
        zxlogf(ERROR, "FakeAmlGpio::Create: unsupported SOC PID %u", info.pid);
        return nullptr;
    }

    fbl::AllocChecker ac;

    fbl::Array<uint16_t> irq_info(new (&ac) uint16_t[info.irq_count], info.irq_count);
    if (!ac.check()) {
      zxlogf(ERROR, "FakeAmlGpio::Create: irq_info alloc failed");
      return nullptr;
    }
    for (uint32_t i = 0; i < info.irq_count; i++) {
      irq_info[i] = 255 + 1;
    }  // initialize irq_info

    fdf::MmioBuffer mmio_gpio(mock_mmio_gpio->GetMmioBuffer());
    fdf::MmioBuffer mmio_gpio_a0(mock_mmio_gpio_a0->GetMmioBuffer());
    fdf::MmioBuffer mmio_interrupt(mock_mmio_interrupt->GetMmioBuffer());

    FakeAmlGpio *device(new (&ac) FakeAmlGpio(
        std::move(mmio_gpio), std::move(mmio_gpio_a0), std::move(mmio_interrupt), gpio_blocks,
        gpio_interrupt, block_count, std::move(info), std::move(irq_info)));
    if (!ac.check()) {
      zxlogf(ERROR, "FakeAmlGpio::Create: device object alloc failed");
      return nullptr;
    }

    return device;
  }

 private:
  explicit FakeAmlGpio(fdf::MmioBuffer mock_mmio_gpio, fdf::MmioBuffer mock_mmio_gpio_a0,
                       fdf::MmioBuffer mock_mmio_interrupt, const AmlGpioBlock *gpio_blocks,
                       const AmlGpioInterrupt *gpio_interrupt, size_t block_count,
                       pdev_device_info_t info, fbl::Array<uint16_t> irq_info)
      : AmlGpio(&fake_proto, std::move(mock_mmio_gpio), std::move(mock_mmio_gpio_a0),
                std::move(mock_mmio_interrupt), gpio_blocks, gpio_interrupt, block_count,
                std::move(info), std::move(irq_info)) {}
};

class A113AmlGpioTest : public zxtest::Test {
 public:
  void SetUp() override {
    fbl::AllocChecker ac;

    gpio_regs_ = fbl::Array(new (&ac) ddk_mock::MockMmioReg[kGpioRegSize], kGpioRegSize);
    if (!ac.check()) {
      zxlogf(ERROR, "AmlGpioTest::SetUp: gpio_regs_ alloc failed");
      return;
    }
    mock_mmio_gpio_ =
        new (&ac) ddk_mock::MockMmioRegRegion(gpio_regs_.get(), sizeof(uint32_t), kGpioRegSize);
    if (!ac.check()) {
      zxlogf(ERROR, "AmlGpioTest::SetUp: mock_mmio_gpio_ alloc failed");
      return;
    }
    gpio_a0_regs_ = fbl::Array(new (&ac) ddk_mock::MockMmioReg[kGpioRegSize], kGpioRegSize);
    if (!ac.check()) {
      zxlogf(ERROR, "AmlGpioTest::SetUp: gpio_a0_regs_ alloc failed");
      return;
    }
    mock_mmio_gpio_a0_ =
        new (&ac) ddk_mock::MockMmioRegRegion(gpio_a0_regs_.get(), sizeof(uint32_t), kGpioRegSize);
    if (!ac.check()) {
      zxlogf(ERROR, "AmlGpioTest::SetUp: mock_mmio_gpio_a0_ alloc failed");
      return;
    }
    interrupt_regs_ =
        fbl::Array(new (&ac) ddk_mock::MockMmioReg[kInterruptRegSize], kInterruptRegSize);
    if (!ac.check()) {
      zxlogf(ERROR, "AmlGpioTest::SetUp: interrupt_regs_ alloc failed");
      return;
    }
    mock_mmio_interrupt_ = new (&ac) ddk_mock::MockMmioRegRegion(
        interrupt_regs_.get(), sizeof(uint32_t), kInterruptRegSize, kInterruptRegOffset);
    if (!ac.check()) {
      zxlogf(ERROR, "AmlGpioTest::SetUp: mock_mmio_interrupt_ alloc failed");
      return;
    }

    // make-up info, pid and irq_count needed for Create
    auto info = pdev_device_info{0, PDEV_PID_AMLOGIC_A113, 0, 2, 3, 0, 0, 0, {0}, "fake_info"};

    gpio_ = FakeAmlGpio::Create(info, mock_mmio_gpio_, mock_mmio_gpio_a0_, mock_mmio_interrupt_);
  }

  void TearDown() override {
    delete mock_mmio_gpio_;
    delete mock_mmio_gpio_a0_;
    delete mock_mmio_interrupt_;
    delete gpio_;
  }

 protected:
  FakeAmlGpio *gpio_;
  fbl::Array<ddk_mock::MockMmioReg> gpio_regs_;
  fbl::Array<ddk_mock::MockMmioReg> gpio_a0_regs_;
  fbl::Array<ddk_mock::MockMmioReg> interrupt_regs_;
  ddk_mock::MockMmioRegRegion *mock_mmio_gpio_;
  ddk_mock::MockMmioRegRegion *mock_mmio_gpio_a0_;
  ddk_mock::MockMmioRegRegion *mock_mmio_interrupt_;
};

class S905d2AmlGpioTest : public zxtest::Test {
 public:
  void SetUp() override {
    fbl::AllocChecker ac;

    gpio_regs_ = fbl::Array(new (&ac) ddk_mock::MockMmioReg[kGpioRegSize], kGpioRegSize);
    if (!ac.check()) {
      zxlogf(ERROR, "AmlGpioTest::SetUp: gpio_regs_ alloc failed");
      return;
    }
    mock_mmio_gpio_ =
        new (&ac) ddk_mock::MockMmioRegRegion(gpio_regs_.get(), sizeof(uint32_t), kGpioRegSize);
    if (!ac.check()) {
      zxlogf(ERROR, "AmlGpioTest::SetUp: mock_mmio_gpio_ alloc failed");
      return;
    }
    gpio_a0_regs_ = fbl::Array(new (&ac) ddk_mock::MockMmioReg[kGpioRegSize], kGpioRegSize);
    if (!ac.check()) {
      zxlogf(ERROR, "AmlGpioTest::SetUp: gpio_a0_regs_ alloc failed");
      return;
    }
    mock_mmio_gpio_a0_ =
        new (&ac) ddk_mock::MockMmioRegRegion(gpio_a0_regs_.get(), sizeof(uint32_t), kGpioRegSize);
    if (!ac.check()) {
      zxlogf(ERROR, "AmlGpioTest::SetUp: mock_mmio_gpio_a0_ alloc failed");
      return;
    }
    interrupt_regs_ =
        fbl::Array(new (&ac) ddk_mock::MockMmioReg[kInterruptRegSize], kInterruptRegSize);
    if (!ac.check()) {
      zxlogf(ERROR, "AmlGpioTest::SetUp: interrupt_regs_ alloc failed");
      return;
    }
    mock_mmio_interrupt_ = new (&ac) ddk_mock::MockMmioRegRegion(
        interrupt_regs_.get(), sizeof(uint32_t), kInterruptRegSize, kInterruptRegOffset);
    if (!ac.check()) {
      zxlogf(ERROR, "AmlGpioTest::SetUp: mock_mmio_interrupt_ alloc failed");
      return;
    }

    // make-up info, pid and irq_count needed for Create
    auto info = pdev_device_info{0, PDEV_PID_AMLOGIC_S905D2, 0, 2, 3, 0, 0, 0, {0}, "fake_info"};

    gpio_ = FakeAmlGpio::Create(info, mock_mmio_gpio_, mock_mmio_gpio_a0_, mock_mmio_interrupt_);
  }

  void TearDown() override {
    delete mock_mmio_gpio_;
    delete mock_mmio_gpio_a0_;
    delete mock_mmio_interrupt_;
    delete gpio_;
  }

 protected:
  FakeAmlGpio *gpio_;
  fbl::Array<ddk_mock::MockMmioReg> gpio_regs_;
  fbl::Array<ddk_mock::MockMmioReg> gpio_a0_regs_;
  fbl::Array<ddk_mock::MockMmioReg> interrupt_regs_;
  ddk_mock::MockMmioRegRegion *mock_mmio_gpio_;
  ddk_mock::MockMmioRegRegion *mock_mmio_gpio_a0_;
  ddk_mock::MockMmioRegRegion *mock_mmio_interrupt_;
};

// GpioImplSetAltFunction Tests
TEST_F(A113AmlGpioTest, A113AltMode1) {
  (*mock_mmio_gpio_)[0x24 * sizeof(uint32_t)].ExpectRead(0x00000000).ExpectWrite(0x00000001);
  EXPECT_OK(gpio_->GpioImplSetAltFunction(0x00, 1));
  mock_mmio_gpio_->VerifyAll();
  mock_mmio_gpio_a0_->VerifyAll();
  mock_mmio_interrupt_->VerifyAll();
}

TEST_F(A113AmlGpioTest, A113AltMode2) {
  (*mock_mmio_gpio_)[0x26 * sizeof(uint32_t)]
      .ExpectRead(0x00000009 << 8)
      .ExpectWrite(0x00000005 << 8);
  EXPECT_OK(gpio_->GpioImplSetAltFunction(0x12, 5));
  mock_mmio_gpio_->VerifyAll();
  mock_mmio_gpio_a0_->VerifyAll();
  mock_mmio_interrupt_->VerifyAll();
}

TEST_F(A113AmlGpioTest, A113AltMode3) {
  (*mock_mmio_gpio_a0_)[0x05 * sizeof(uint32_t)]
      .ExpectRead(0x00000000)
      .ExpectWrite(0x00000005 << 16);
  EXPECT_OK(gpio_->GpioImplSetAltFunction(0x56, 5));
  mock_mmio_gpio_->VerifyAll();
  mock_mmio_gpio_a0_->VerifyAll();
  mock_mmio_interrupt_->VerifyAll();
}

TEST_F(S905d2AmlGpioTest, S905d2AltMode) {
  (*mock_mmio_gpio_)[0xb6 * sizeof(uint32_t)].ExpectRead(0x00000000).ExpectWrite(0x00000001);
  EXPECT_OK(gpio_->GpioImplSetAltFunction(0x00, 1));
  mock_mmio_gpio_->VerifyAll();
  mock_mmio_gpio_a0_->VerifyAll();
  mock_mmio_interrupt_->VerifyAll();
}

TEST_F(A113AmlGpioTest, AltModeFail1) {
  EXPECT_NOT_OK(gpio_->GpioImplSetAltFunction(0x00, 16));
  mock_mmio_gpio_->VerifyAll();
  mock_mmio_gpio_a0_->VerifyAll();
  mock_mmio_interrupt_->VerifyAll();
}

TEST_F(A113AmlGpioTest, AltModeFail2) {
  EXPECT_NOT_OK(gpio_->GpioImplSetAltFunction(0xFFFF, 1));
  mock_mmio_gpio_->VerifyAll();
  mock_mmio_gpio_a0_->VerifyAll();
  mock_mmio_interrupt_->VerifyAll();
}

// GpioImplConfigIn Tests
TEST_F(A113AmlGpioTest, A113NoPull0) {
  (*mock_mmio_gpio_)[0x12 * sizeof(uint32_t)]
      .ExpectRead(0xFFFFFFFF)
      .ExpectWrite(0xFFFFFFFF);  // oen
  (*mock_mmio_gpio_)[0x3c * sizeof(uint32_t)]
      .ExpectRead(0xFFFFFFFF)
      .ExpectWrite(0xFFFFFFFF);  // pull
  (*mock_mmio_gpio_)[0x4a * sizeof(uint32_t)]
      .ExpectRead(0xFFFFFFFF)
      .ExpectWrite(0xFFFFFFFE);  // pull_en
  EXPECT_OK(gpio_->GpioImplConfigIn(0, GPIO_NO_PULL));
  mock_mmio_gpio_->VerifyAll();
  mock_mmio_gpio_a0_->VerifyAll();
  mock_mmio_interrupt_->VerifyAll();
}

TEST_F(A113AmlGpioTest, A113NoPullMid) {
  (*mock_mmio_gpio_)[0x12 * sizeof(uint32_t)]
      .ExpectRead(0xFFFFFFFF)
      .ExpectWrite(0xFFFFFFFF);  // oen
  (*mock_mmio_gpio_)[0x3c * sizeof(uint32_t)]
      .ExpectRead(0xFFFFFFFF)
      .ExpectWrite(0xFFFFFFFF);  // pull
  (*mock_mmio_gpio_)[0x4a * sizeof(uint32_t)]
      .ExpectRead(0xFFFFFFFF)
      .ExpectWrite(0xFFFBFFFF);  // pull_en
  EXPECT_OK(gpio_->GpioImplConfigIn(0x12, GPIO_NO_PULL));
  mock_mmio_gpio_->VerifyAll();
  mock_mmio_gpio_a0_->VerifyAll();
  mock_mmio_interrupt_->VerifyAll();
}

TEST_F(A113AmlGpioTest, A113NoPullHigh) {
  (*mock_mmio_gpio_a0_)[0x08 * sizeof(uint32_t)]
      .ExpectRead(0xFFFFFFFF)
      .ExpectWrite(0xFFFFFFFF);  // oen
  (*mock_mmio_gpio_a0_)[0x0b * sizeof(uint32_t)]
      .ExpectRead(0xFFFFFFFF)
      .ExpectWrite(0xFFFFFFFF);  // pull
  (*mock_mmio_gpio_a0_)[0x0b * sizeof(uint32_t)]
      .ExpectRead(0xFFFFFFFF)
      .ExpectWrite(0xFFFEFFFF);  // pull_en
  EXPECT_OK(gpio_->GpioImplConfigIn(0x56, GPIO_NO_PULL));
  mock_mmio_gpio_->VerifyAll();
  mock_mmio_gpio_a0_->VerifyAll();
  mock_mmio_interrupt_->VerifyAll();
}

TEST_F(S905d2AmlGpioTest, S905d2NoPull0) {
  (*mock_mmio_gpio_)[0x1c * sizeof(uint32_t)]
      .ExpectRead(0xFFFFFFFF)
      .ExpectWrite(0xFFFFFFFF);  // oen
  (*mock_mmio_gpio_)[0x3e * sizeof(uint32_t)]
      .ExpectRead(0xFFFFFFFF)
      .ExpectWrite(0xFFFFFFFF);  // pull
  (*mock_mmio_gpio_)[0x4c * sizeof(uint32_t)]
      .ExpectRead(0xFFFFFFFF)
      .ExpectWrite(0xFFFFFFFE);  // pull_en
  EXPECT_OK(gpio_->GpioImplConfigIn(0, GPIO_NO_PULL));
  mock_mmio_gpio_->VerifyAll();
  mock_mmio_gpio_a0_->VerifyAll();
  mock_mmio_interrupt_->VerifyAll();
}

TEST_F(S905d2AmlGpioTest, S905d2PullUp) {
  (*mock_mmio_gpio_)[0x10 * sizeof(uint32_t)]
      .ExpectRead(0xFFFFFFFF)
      .ExpectWrite(0xFFFFFFFF);  // oen
  (*mock_mmio_gpio_)[0x3a * sizeof(uint32_t)]
      .ExpectRead(0xFFFFFFFF)
      .ExpectWrite(0xFFFFFFFF);  // pull
  (*mock_mmio_gpio_)[0x48 * sizeof(uint32_t)]
      .ExpectRead(0xFFFFFFFF)
      .ExpectWrite(0xFFFFFFFF);  // pull_en
  EXPECT_OK(gpio_->GpioImplConfigIn(0x21, GPIO_PULL_UP));
  mock_mmio_gpio_->VerifyAll();
  mock_mmio_gpio_a0_->VerifyAll();
  mock_mmio_interrupt_->VerifyAll();
}

TEST_F(S905d2AmlGpioTest, S905d2PullDown) {
  (*mock_mmio_gpio_)[0x10 * sizeof(uint32_t)]
      .ExpectRead(0xFFFFFFFF)
      .ExpectWrite(0xFFFFFFFF);  // oen
  (*mock_mmio_gpio_)[0x3a * sizeof(uint32_t)]
      .ExpectRead(0xFFFFFFFF)
      .ExpectWrite(0xFFFFFFFE);  // pull
  (*mock_mmio_gpio_)[0x48 * sizeof(uint32_t)]
      .ExpectRead(0xFFFFFFFF)
      .ExpectWrite(0xFFFFFFFF);  // pull_en
  EXPECT_OK(gpio_->GpioImplConfigIn(0x20, GPIO_PULL_DOWN));
  mock_mmio_gpio_->VerifyAll();
  mock_mmio_gpio_a0_->VerifyAll();
  mock_mmio_interrupt_->VerifyAll();
}

TEST_F(A113AmlGpioTest, A113NoPullFail) {
  EXPECT_NOT_OK(gpio_->GpioImplConfigIn(0xFFFF, GPIO_NO_PULL));
  mock_mmio_gpio_->VerifyAll();
  mock_mmio_gpio_a0_->VerifyAll();
  mock_mmio_interrupt_->VerifyAll();
}

// GpioImplConfigOut Tests
TEST_F(A113AmlGpioTest, A113Out) {
  (*mock_mmio_gpio_)[0x0d * sizeof(uint32_t)]
      .ExpectRead(0xFFFFFFFF)
      .ExpectWrite(0xFFFFFFFF);  // output
  (*mock_mmio_gpio_)[0x0c * sizeof(uint32_t)]
      .ExpectRead(0xFFFFFFFF)
      .ExpectWrite(0xFFFFFFFB);  // oen
  EXPECT_OK(gpio_->GpioImplConfigOut(0x19, 1));
  mock_mmio_gpio_->VerifyAll();
  mock_mmio_gpio_a0_->VerifyAll();
  mock_mmio_interrupt_->VerifyAll();
}

// GpioImplRead Tests
TEST_F(A113AmlGpioTest, A113Read) {
  (*mock_mmio_gpio_)[0x12 * sizeof(uint32_t)]
      .ExpectRead(0xFFFFFFFF)
      .ExpectWrite(0xFFFFFFFF);  // oen
  (*mock_mmio_gpio_)[0x3c * sizeof(uint32_t)]
      .ExpectRead(0xFFFFFFFF)
      .ExpectWrite(0xFFFFFFFF);  // pull
  (*mock_mmio_gpio_)[0x4a * sizeof(uint32_t)]
      .ExpectRead(0xFFFFFFFF)
      .ExpectWrite(0xFFFFFFDF);  // pull_en
  EXPECT_OK(gpio_->GpioImplConfigIn(5, GPIO_NO_PULL));
  (*mock_mmio_gpio_)[0x14 * sizeof(uint32_t)].ExpectRead(0x00000020);  // read 0x01.
  (*mock_mmio_gpio_)[0x14 * sizeof(uint32_t)].ExpectRead(0x00000000);  // read 0x00.
  (*mock_mmio_gpio_)[0x14 * sizeof(uint32_t)].ExpectRead(0x00000020);  // read 0x01.
  uint8_t out_value = 0;
  EXPECT_OK(gpio_->GpioImplRead(5, &out_value));
  EXPECT_EQ(out_value, 0x01);
  EXPECT_OK(gpio_->GpioImplRead(5, &out_value));
  EXPECT_EQ(out_value, 0x00);
  EXPECT_OK(gpio_->GpioImplRead(5, &out_value));
  EXPECT_EQ(out_value, 0x01);
  mock_mmio_gpio_->VerifyAll();
  mock_mmio_gpio_a0_->VerifyAll();
  mock_mmio_interrupt_->VerifyAll();
}

// GpioImplWrite Tests
TEST_F(A113AmlGpioTest, A113Write) {
  (*mock_mmio_gpio_)[0x13 * sizeof(uint32_t)]
      .ExpectRead(0xFFFFFFFF)
      .ExpectWrite(0xFFFFFFFF);  // write
  (*mock_mmio_gpio_)[0x13 * sizeof(uint32_t)]
      .ExpectRead(0xFFFFFFFF)
      .ExpectWrite(0xFFFFBFFF);  // write
  (*mock_mmio_gpio_)[0x13 * sizeof(uint32_t)]
      .ExpectRead(0xFFFFFFFF)
      .ExpectWrite(0xFFFFFFFF);  // write
  EXPECT_OK(gpio_->GpioImplWrite(14, 200));
  EXPECT_OK(gpio_->GpioImplWrite(14, 0));
  EXPECT_OK(gpio_->GpioImplWrite(14, 92));
  mock_mmio_gpio_->VerifyAll();
  mock_mmio_gpio_a0_->VerifyAll();
  mock_mmio_interrupt_->VerifyAll();
}

// GpioImplGetInterrupt Tests
TEST_F(A113AmlGpioTest, A113GetInterruptFail) {
  zx::interrupt out_int;
  EXPECT_NOT_OK(gpio_->GpioImplGetInterrupt(0xFFFF, ZX_INTERRUPT_MODE_EDGE_LOW, &out_int));
  mock_mmio_gpio_->VerifyAll();
  mock_mmio_gpio_a0_->VerifyAll();
  mock_mmio_interrupt_->VerifyAll();
}

TEST_F(A113AmlGpioTest, A113GetInterrupt) {
  (*mock_mmio_interrupt_)[0x3c21 * sizeof(uint32_t)]
      .ExpectRead(0x00000000)
      .ExpectWrite(0x00000048);  // modify
  (*mock_mmio_interrupt_)[0x3c20 * sizeof(uint32_t)].ExpectRead(0x00000000).ExpectWrite(0x00010001);
  (*mock_mmio_interrupt_)[0x3c23 * sizeof(uint32_t)].ExpectRead(0x00000000).ExpectWrite(0x00000007);
  zx::interrupt out_int;
  EXPECT_OK(gpio_->GpioImplGetInterrupt(0x0B, ZX_INTERRUPT_MODE_EDGE_LOW, &out_int));
  mock_mmio_gpio_->VerifyAll();
  mock_mmio_gpio_a0_->VerifyAll();
  mock_mmio_interrupt_->VerifyAll();
}

// GpioImplReleaseInterrupt Tests
TEST_F(A113AmlGpioTest, A113ReleaseInterruptFail) {
  EXPECT_NOT_OK(gpio_->GpioImplReleaseInterrupt(0x66));
  mock_mmio_gpio_->VerifyAll();
  mock_mmio_gpio_a0_->VerifyAll();
  mock_mmio_interrupt_->VerifyAll();
}

TEST_F(A113AmlGpioTest, A113ReleaseInterrupt) {
  (*mock_mmio_interrupt_)[0x3c21 * sizeof(uint32_t)]
      .ExpectRead(0x00000000)
      .ExpectWrite(0x00000048);  // modify
  (*mock_mmio_interrupt_)[0x3c20 * sizeof(uint32_t)].ExpectRead(0x00000000).ExpectWrite(0x00010001);
  (*mock_mmio_interrupt_)[0x3c23 * sizeof(uint32_t)].ExpectRead(0x00000000).ExpectWrite(0x00000007);
  zx::interrupt out_int;
  EXPECT_OK(gpio_->GpioImplGetInterrupt(0x0B, ZX_INTERRUPT_MODE_EDGE_LOW, &out_int));
  EXPECT_OK(gpio_->GpioImplReleaseInterrupt(0x0B));
  mock_mmio_gpio_->VerifyAll();
  mock_mmio_gpio_a0_->VerifyAll();
  mock_mmio_interrupt_->VerifyAll();
}

// GpioImplSetPolarity Tests
TEST_F(A113AmlGpioTest, A113InterruptSetPolarityEdge) {
  (*mock_mmio_interrupt_)[0x3c21 * sizeof(uint32_t)]
      .ExpectRead(0x00000000)
      .ExpectWrite(0x00000048);  // modify
  (*mock_mmio_interrupt_)[0x3c20 * sizeof(uint32_t)].ExpectRead(0x00000000).ExpectWrite(0x00010001);
  (*mock_mmio_interrupt_)[0x3c23 * sizeof(uint32_t)].ExpectRead(0x00000000).ExpectWrite(0x00000007);
  zx::interrupt out_int;
  EXPECT_OK(gpio_->GpioImplGetInterrupt(0x0B, ZX_INTERRUPT_MODE_EDGE_LOW, &out_int));

  (*mock_mmio_interrupt_)[0x3c20 * sizeof(uint32_t)]
      .ExpectRead(0x00010001)
      .ExpectWrite(0x00000001);  // polarity + for any edge.
  EXPECT_OK(gpio_->GpioImplSetPolarity(0x0B, true));
  mock_mmio_gpio_->VerifyAll();
  mock_mmio_gpio_a0_->VerifyAll();
  mock_mmio_interrupt_->VerifyAll();
}

// GpioImplSetDriveStrength Tests
TEST_F(A113AmlGpioTest, A113SetDriveStrength) {
  EXPECT_NOT_OK(gpio_->GpioImplSetDriveStrength(0x87, 2, nullptr));
  mock_mmio_gpio_->VerifyAll();
  mock_mmio_gpio_a0_->VerifyAll();
  mock_mmio_interrupt_->VerifyAll();
}

TEST_F(S905d2AmlGpioTest, S905d2SetDriveStrength) {
  (*mock_mmio_gpio_a0_)[0x08 * sizeof(uint32_t)].ExpectRead(0xFFFFFFFF).ExpectWrite(0xFFFFFFFB);
  uint64_t actual = 0;
  EXPECT_OK(gpio_->GpioImplSetDriveStrength(0x62, 3000, &actual));
  EXPECT_EQ(actual, 3000);
  mock_mmio_gpio_->VerifyAll();
  mock_mmio_gpio_a0_->VerifyAll();
  mock_mmio_interrupt_->VerifyAll();
}

TEST_F(S905d2AmlGpioTest, S905d2GetDriveStrength) {
  (*mock_mmio_gpio_a0_)[0x08 * sizeof(uint32_t)].ExpectRead(0xFFFFFFFB);
  uint64_t result = 0;
  EXPECT_OK(gpio_->GpioImplGetDriveStrength(0x62, &result));
  EXPECT_EQ(result, 3000);
  mock_mmio_gpio_->VerifyAll();
  mock_mmio_gpio_a0_->VerifyAll();
  mock_mmio_interrupt_->VerifyAll();
}

}  // namespace gpio

int main(int argc, char **argv) { return RUN_ALL_TESTS(argc, argv); }
