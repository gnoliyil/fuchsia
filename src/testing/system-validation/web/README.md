# Web System Validation Test

This directory contains sample apps that exercises `fuchsia.web` API's, and system validation tests that executes the demo apps.

System validation sets up the test with following capability routing relationships:

```
        test_manager <-- root
                |  parent to
                V
        system_validation_test_realm (facet: system-validation) <-- system validation test root
                |  parent to
                V
        test component (ex: `web_view_system_validation`)
         /  parent to  |           \
        /              |            \
        V              V             V
    sample-app  <-->  web_engine    file_server (startup: eager)
(ex: `web_view.cm`)
```

# Run existing tests

1. Build `workstation_eng_paused` product with system validation test targets.

```
fx set workstation_eng_paused.qemu-x64 --release --with-base //src/chromium:web_engine --with //src/testing/system-validation:tests
fx build
```

2. Start the emulator and package server

```
ffx emu start
fx serve
```

3. Run test

```
$ fx test fuchsia-pkg://fuchsia.com/dynamic_elements_web_system_validation#meta/web_view_system_validation.cm --ffx-output-directory /path/to/output/dir
$ fx test fuchsia-pkg://fuchsia.com/simple_png_web_system_validation#meta/web_view_system_validation.cm --ffx-output-directory /path/to/output/dir
$ fx test fuchsia-pkg://fuchsia.com/simple_video_web_system_validation#meta/web_view_system_validation.cm --ffx-output-directory /path/to/output/dir
```