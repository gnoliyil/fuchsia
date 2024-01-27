// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <string.h>
#include <zircon/errors.h>

#include <hid/ft3x27.h>

static const uint8_t ft3x27_touch_report_desc[] = {
    0x05, 0x0D,        // Usage Page (Digitizer)
    0x09, 0x04,        // Usage (Touch Screen)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x01,        //   Report ID (1)
    0x09, 0x22,        //   Usage (Finger)
    0xA1, 0x02,        //   Collection (Logical)
    0x09, 0x42,        //     Usage (Tip Switch)
    0x15, 0x00,        //     Logical Minimum (0)
    0x25, 0x01,        //     Logical Maximum (1)
    0x75, 0x01,        //     Report Size (1)
    0x95, 0x01,        //     Report Count (1)
    0x81, 0x02,        //     Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x75, 0x01,        //     Report Size (1)
    0x81, 0x03,        //     Input (Const,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x75, 0x06,        //     Report Size (6)
    0x09, 0x51,        //     Usage (0x51)
    0x25, 0x3F,        //     Logical Maximum (63)
    0x81, 0x02,        //     Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x95, 0x01,        //     Report Count (1)
    0x05, 0x01,        //     Usage Page (Generic Desktop Ctrls)
    0xA4,              //     Push
    0x26, 0x58, 0x02,  //       Logical Maximum (600)
    0x75, 0x10,        //       Report Size (16)
    0x09, 0x30,        //       Usage (X)
    0x95, 0x01,        //       Report Count (1)
    0x81, 0x02,        //       Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x26, 0x00, 0x04,  //       Logical Maximum (1024)
    0x09, 0x31,        //       Usage (Y)
    0x81, 0x02,        //       Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0xB4,              //     Pop
    0xC0,              //   End Collection
    0x05, 0x0D,        //   Usage Page (Digitizer)
    0x09, 0x22,        //   Usage (Finger)
    0xA1, 0x02,        //   Collection (Logical)
    0x09, 0x42,        //     Usage (Tip Switch)
    0x15, 0x00,        //     Logical Minimum (0)
    0x25, 0x01,        //     Logical Maximum (1)
    0x75, 0x01,        //     Report Size (1)
    0x95, 0x01,        //     Report Count (1)
    0x81, 0x02,        //     Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x75, 0x01,        //     Report Size (1)
    0x81, 0x03,        //     Input (Const,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x75, 0x06,        //     Report Size (6)
    0x09, 0x51,        //     Usage (0x51)
    0x25, 0x3F,        //     Logical Maximum (63)
    0x81, 0x02,        //     Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x95, 0x01,        //     Report Count (1)
    0x05, 0x01,        //     Usage Page (Generic Desktop Ctrls)
    0xA4,              //     Push
    0x26, 0x58, 0x02,  //       Logical Maximum (600)
    0x75, 0x10,        //       Report Size (16)
    0x09, 0x30,        //       Usage (X)
    0x95, 0x01,        //       Report Count (1)
    0x81, 0x02,        //       Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x26, 0x00, 0x04,  //       Logical Maximum (1024)
    0x09, 0x31,        //       Usage (Y)
    0x81, 0x02,        //       Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0xB4,              //     Pop
    0xC0,              //   End Collection
    0x05, 0x0D,        //   Usage Page (Digitizer)
    0x09, 0x22,        //   Usage (Finger)
    0xA1, 0x02,        //   Collection (Logical)
    0x09, 0x42,        //     Usage (Tip Switch)
    0x15, 0x00,        //     Logical Minimum (0)
    0x25, 0x01,        //     Logical Maximum (1)
    0x75, 0x01,        //     Report Size (1)
    0x95, 0x01,        //     Report Count (1)
    0x81, 0x02,        //     Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x75, 0x01,        //     Report Size (1)
    0x81, 0x03,        //     Input (Const,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x75, 0x06,        //     Report Size (6)
    0x09, 0x51,        //     Usage (0x51)
    0x25, 0x3F,        //     Logical Maximum (63)
    0x81, 0x02,        //     Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x95, 0x01,        //     Report Count (1)
    0x05, 0x01,        //     Usage Page (Generic Desktop Ctrls)
    0xA4,              //     Push
    0x26, 0x58, 0x02,  //       Logical Maximum (600)
    0x75, 0x10,        //       Report Size (16)
    0x09, 0x30,        //       Usage (X)
    0x95, 0x01,        //       Report Count (1)
    0x81, 0x02,        //       Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x26, 0x00, 0x04,  //       Logical Maximum (1024)
    0x09, 0x31,        //       Usage (Y)
    0x81, 0x02,        //       Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0xB4,              //     Pop
    0xC0,              //   End Collection
    0x05, 0x0D,        //   Usage Page (Digitizer)
    0x09, 0x22,        //   Usage (Finger)
    0xA1, 0x02,        //   Collection (Logical)
    0x09, 0x42,        //     Usage (Tip Switch)
    0x15, 0x00,        //     Logical Minimum (0)
    0x25, 0x01,        //     Logical Maximum (1)
    0x75, 0x01,        //     Report Size (1)
    0x95, 0x01,        //     Report Count (1)
    0x81, 0x02,        //     Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x75, 0x01,        //     Report Size (1)
    0x81, 0x03,        //     Input (Const,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x75, 0x06,        //     Report Size (6)
    0x09, 0x51,        //     Usage (0x51)
    0x25, 0x3F,        //     Logical Maximum (63)
    0x81, 0x02,        //     Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x95, 0x01,        //     Report Count (1)
    0x05, 0x01,        //     Usage Page (Generic Desktop Ctrls)
    0xA4,              //     Push
    0x26, 0x58, 0x02,  //       Logical Maximum (600)
    0x75, 0x10,        //       Report Size (16)
    0x09, 0x30,        //       Usage (X)
    0x95, 0x01,        //       Report Count (1)
    0x81, 0x02,        //       Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x26, 0x00, 0x04,  //       Logical Maximum (1024)
    0x09, 0x31,        //       Usage (Y)
    0x81, 0x02,        //       Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0xB4,              //     Pop
    0xC0,              //   End Collection
    0x05, 0x0D,        //   Usage Page (Digitizer)
    0x09, 0x22,        //   Usage (Finger)
    0xA1, 0x02,        //   Collection (Logical)
    0x09, 0x42,        //     Usage (Tip Switch)
    0x15, 0x00,        //     Logical Minimum (0)
    0x25, 0x01,        //     Logical Maximum (1)
    0x75, 0x01,        //     Report Size (1)
    0x95, 0x01,        //     Report Count (1)
    0x81, 0x02,        //     Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x75, 0x01,        //     Report Size (1)
    0x81, 0x03,        //     Input (Const,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x75, 0x06,        //     Report Size (6)
    0x09, 0x51,        //     Usage (0x51)
    0x25, 0x3F,        //     Logical Maximum (63)
    0x81, 0x02,        //     Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x95, 0x01,        //     Report Count (1)
    0x05, 0x01,        //     Usage Page (Generic Desktop Ctrls)
    0xA4,              //     Push
    0x26, 0x58, 0x02,  //       Logical Maximum (600)
    0x75, 0x10,        //       Report Size (16)
    0x09, 0x30,        //       Usage (X)
    0x95, 0x01,        //       Report Count (1)
    0x81, 0x02,        //       Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x26, 0x00, 0x04,  //       Logical Maximum (1024)
    0x09, 0x31,        //       Usage (Y)
    0x81, 0x02,        //       Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0xB4,              //     Pop
    0xC0,              //   End Collection
    0x05, 0x0D,        //   Usage Page (Digitizer)
    0x09, 0x54,        //   Usage (0x54)
    0x25, 0x05,        //   Logical Maximum (5)
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x08,        //   Report Size (8)
    0x81, 0x02,        //   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0xC0,              // End Collection
};

bool is_ft3x27_touch_report_desc(const uint8_t* data, size_t len) {
  if (!data)
    return false;

  if (len != sizeof(ft3x27_touch_report_desc))
    return false;

  return (memcmp(data, ft3x27_touch_report_desc, len) == 0);
}

zx_status_t setup_ft3x27_touch(int fd) {
  if (fd < 0)
    return ZX_ERR_INVALID_ARGS;

  return ZX_OK;
}

size_t get_ft3x27_report_desc(const uint8_t** buf) {
  *buf = ft3x27_touch_report_desc;
  return sizeof(ft3x27_touch_report_desc);
}
