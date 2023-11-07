# Copyright 2023 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import asyncio
import json
import typing
import subprocess

import fidl.fuchsia_tracing_controller as tracing_controller
import fidl.fuchsia_tracing as tracing
from fidl import AsyncSocket
from fuchsia_controller_py import ZxStatus, Socket
from mobly import asserts
from mobly import base_test
from mobly import test_runner
from mobly_controller import fuchsia_device
from mobly_controller.fuchsia_device import asynctest


TRACE2JSON = "host-tools/trace2json"


class FuchsiaControllerTests(base_test.BaseTestClass):
    def setup_class(self) -> None:
        self.fuchsia_devices: typing.List[
            fuchsia_device.FuchsiaDevice
        ] = self.register_controller(fuchsia_device)
        self.device = self.fuchsia_devices[0]
        self.device.set_ctx(self)

    @asynctest
    async def test_fuchsia_device_get_known_categories(self) -> None:
        """Verifies that kernel:vm is an existing category for tracing on the device."""
        ch = self.device.ctx.connect_device_proxy(
            "core/trace_manager", tracing_controller.Controller.MARKER
        )
        controller = tracing_controller.Controller.Client(ch)
        categories = (await controller.get_known_categories()).categories
        found_kernel_category = False
        for category in categories:
            if category.name == "kernel:vm":
                found_kernel_category = True
                break
        asserts.assert_true(
            found_kernel_category,
            msg="Was not able to find 'kernel.vm' category in known output",
        )

    @asynctest
    async def test_fuchsia_device_tracing_start_stop(self) -> None:
        """Does a simple start and stop of tracing on a device."""
        ch = self.device.ctx.connect_device_proxy(
            "core/trace_manager", tracing_controller.Controller.MARKER
        )
        controller = tracing_controller.Controller.Client(ch)
        categories = [
            "blobfs",
            "gfx",
            "system_metrics",
        ]
        config = tracing_controller.TraceConfig(
            buffer_size_megabytes_hint=4,
            categories=categories,
            buffering_mode=tracing.BufferingMode.ONESHOT,
        )
        client, server = Socket.create()
        client = AsyncSocket(client)

        controller.initialize_tracing(config=config, output=server.take())
        await controller.start_tracing(
            options=tracing_controller.StartOptions()
        )
        socket_task = asyncio.get_running_loop().create_task(client.read_all())
        await asyncio.sleep(10)
        await controller.stop_tracing(
            options=tracing_controller.StopOptions(write_results=True)
        )
        res = await controller.terminate_tracing(
            options=tracing_controller.TerminateOptions(write_results=True)
        )

        terminate_result = res.result
        asserts.assert_true(
            len(terminate_result.provider_stats) > 0,
            msg="Terminate result provider stats should not be empty.",
        )
        raw_trace = await socket_task
        asserts.assert_equal(type(raw_trace), bytearray)
        asserts.assert_true(
            len(raw_trace) > 0, msg="Output bytes should not be empty."
        )
        ps = subprocess.Popen(
            [TRACE2JSON], stdin=subprocess.PIPE, stdout=subprocess.PIPE
        )
        js, _ = ps.communicate(input=raw_trace)
        js_obj = json.loads(js.decode("utf8"))
        ps.kill()
        asserts.assert_true(
            js_obj.get("traceEvents") is not None,
            "Expected traceEvents to be present",
        )
        for trace_event in js_obj["traceEvents"]:
            trace_cat = trace_event["cat"]
            asserts.assert_true(
                trace_cat in categories,
                msg=f"Found unexpected category that isn't part of trace: {trace_cat}",
            )


if __name__ == "__main__":
    test_runner.main()
