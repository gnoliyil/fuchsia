Start the Fuchsia debugger ([`zxdb`][fuchsia-debugger]) in VS Code and debug
the sample component, which is now  updated to crash when it runs.

The tasks include:

- Create a launch configuration for the debugger in VS Code.
- Set a breakpoint in the source code.
- Start the Fuchsia debugger.
- Step through the code.

In VS Code, do the following:

1. Click the **Run and Debug** icon on the left side of VS Code:

   <img class="vscode-image-center"
   alt="This figure shows the Run and Debug option of VS Code."
   src="/docs/reference/tools/editors/vscode/images/extensions/ext-start-debug.png">

1. Click the **Show all automatic debug configurations** link.

   This opens the Command Palette and displays a list of
   launch configurations.

1. In the Command Palette, click
   **Add Config (fuchsia-getting-started)...**.

1. Click **zxdb**.

   This opens the `.vscode/launch.json` file.

1. Update this `launch.json` file to the following configuration:

   ```json5 {:.devsite-disable-click-to-copy}
   {
     "configurations": [
       {
         "name": "{{ '<strong>' }}Fuchsia getting started{{ '</strong>' }}",
         "type": "zxdb",
         "request": "launch",
         "launchCommand": "{{ '<strong>' }}tools/bazel run --config=fuchsia_x64 src/hello_world:pkg.component{{ '</strong>' }}",
         "process": "{{ '<strong>' }}hello_world{{ '</strong>' }}"
       }
     ]
   }
   ```

   This configuration is set to start the `hello_world`
   component and attach the debugger to it.

1. To save the file, press `CTRL+S` (or `CMD+S` on macOS)

1. Click the **Explorer** icon on the left side of VS Code.

1. Open the `src/hello_world/hello_world.cc` file.

1. To set a breakpoint at the `main()` method, click the space left to
   the line number.

   <img class="vscode-image vscode-image-center"
   alt="This figure shows the Run and Debug option of VS Code."
   src="images/get-started-vscode-breakpoint.png">

   When a breakpoint is set, a red dot appears.

1. At the top of the **Run and Debug** panel, select the
   **Fuchsia getting started** launch configuration.

1. At the top of the **Run and Debug** panel, click
   the **Play** icon to launch the debugger.

   This builds and runs the `hello_world` component, which causes
   the debugger to pause at the line where the breakpoint is set
   in the `src/hello_world/hello_world.cc` file.

1. Click the **DEBUG CONSOLE** tab on the VS Code panel.

   <img class="vscode-image vscode-image-center"
   alt="This figure shows the debug console."
   src="images/get-started-vscode-debug-console.png">

   This shows the console output of the Fuchsia debugger (`zxdb`).

1. Click the **FUCHISA LOGS** tab on the VS Code panel.

1. In the **Filter logs...** text box, type `hello_world` and press **Enter**.

   You may see some `Hello, World!` and `Hello again, World!` entries from
   the previous sections. However, you can ignore those entries.

1. At the top right corner of the **FUCHSIA LOGS** panel,
   click the **Clear logs** icon.

1. In the debug toolbar at the top of VS Code, click the **Continue** icon:

   <img class="vscode-image vscode-image-center"
   alt="This figure shows the continue button of the debug toolbar."
   src="images/get-started-vscode-debug-continue.png">

1. In the **FUCHSIA LOGS** panel, verify that a new `Hello again, World!`
   entry is printed in the logs.

1. To exit the debugger, click the **Stop** icon in the debug toolbar.
