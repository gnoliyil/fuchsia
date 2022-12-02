Start the Fuchsia debugger ([`zxdb`][fuchsia-debugger]) in VS Code and debug
the sample component, which is now  updated to crash when it runs.

The tasks include:

- Create a launch configuration for the debugger in VS Code.
- Set a breakpoint in the source code.
- Start the Fuchsia debugger.
- Step through the code.

In VS Code, do the following:

1. Click the **Run and Debug** icon on the left side of VS Code.

   ![Run and Debug](images/get-started-vscode-run-and-debug-icon.png "The Run and Debug icon in VS Code"){: .screenshot width="400"}

1. Click the **Show all automatic debug configurations** link.

   This opens the Command Palette and displays a list of
   launch configurations.

1. In the Command Palette, click
   **Add Config (fuchsia-getting-started)...**.

1. Click **zxdb**.

   This opens the `.vscode/launch.json` file.

1. Update this `launch.json` file to the following configuration:

   ```json5 {:.devsite-disable-click-to-copy}
   "configurations": [
       {
           "name": "{{ '<strong>' }}Fuchsia getting started{{ '</strong>' }}",
           "type": "zxdb",
           "request": "launch",
           "launchCommand": "{{ '<strong>' }}tools/bazel run //src/hello_world:pkg.component{{ '</strong>' }}",
           "process": "{{ '<strong>' }}hello_world{{ '</strong>' }}"
       }
   ]
   ```

   This configuration is set to start the `hello_world`
   component and attach the debugger to it.

1. To save the file, press `CTRL+S` (or `Command+S` on macOS).

1. Select the `src/hello_world/hello_world.cc` file from the **OPEN EDITORS**
   view at the top of VS Code.

1. To set a breakpoint at the `main()` method, click the space to the left of
   the line number.

   ![Breakpoint](images/get-started-vscode-breakpoint.png "A breakpoint in VS Code"){: .screenshot width="500"}

   When a breakpoint is set, a red dot appears.

1. At the top of the **Run and Debug** panel, select
   the **Fuchsia getting started** option in the dropdown memu.

1. At the top of the **Run and Debug** panel, click
   the **Play** icon to launch the debugger.

   ![Play](images/get-started-vscode-debug-play-icon.png "A breakpoint in VS Code"){: .screenshot width="400"}

   This builds and runs the `hello_world` component, which causes
   the debugger to pause at the line where the breakpoint is set
   in the `src/hello_world/hello_world.cc` file.

1. Click the **DEBUG CONSOLE** tab on the VS Code panel.

   ![Debug console](images/get-started-vscode-debug-console.png "The Debug console panel in VS Code"){: .screenshot}

   This shows the console output of the Fuchsia debugger (`zxdb`).

1. Click the **FUCHISA LOGS** tab on the VS Code panel.

1. In the **Filter logs...** text box, type `hello_world` and press **Enter**.

   You may see some `Hello, World!` and `Hello again, World!` entries from
   the previous sections. However, you can ignore those entries.

1. At the top right corner of the **FUCHSIA LOGS** panel,
   click the **Clear logs** icon.

1. In the debug toolbar at the top of VS Code, click the **Step Over** icon.

   ![Step over](images/get-started-vscode-step-over-icon.png "The Step Over icon in VS Code"){: .screenshot width="300"}

1. In the **FUCHSIA LOGS** panel, verify that a new `Hello again, World!`
   entry is printed in the logs.

   ![Hello again](images/get-started-vscode-debug-hello-again-world.png "Hello again, World in the Fuchsia logs panel of VS Code"){: .screenshot}

1. To exit the debugger, click the **Stop** icon in the debug toolbar.

   This causes the component to finish the execution of the rest of the code.
