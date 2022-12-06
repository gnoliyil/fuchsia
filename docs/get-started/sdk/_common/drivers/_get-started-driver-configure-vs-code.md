The SDK driver samples repository includes a `driver-samples.code-workspace`
file. This configuration file sets up a VS Code workspace for the Fuchsia SDK
and recommends Fuchsia-specific VS Code extensions.

In VS Code, do the following:

1. Click **File > Open Workspace from File**.

   This opens the host machine's home directory.

1. Go to your `fuchsia-drivers` directory.
1. Select the `driver-samples.code-workspace` file.
1. Click **Yes, I trust the authors**.

   A pop-up message appears at the bottom right corner of VS Code.

1. Click **Install**.

   This installed the following extensions:

   *  [Fuchsia VS Code extension][fuchsia-vscode-ext]{:.external}
   *  [Bazel extension][bazel-vscode-ext]{:.external}

   Note: You may see more pop-ups recommending other extensions while using
   VS Code. However, you can safely ignore them for the rest of this guide.

<!-- Reference links -->

[bazel-vscode-ext]: https://marketplace.visualstudio.com/items?itemName=BazelBuild.vscode-bazel
[fuchsia-vscode-ext]: https://marketplace.visualstudio.com/items?itemName=fuchsia-authors.vscode-fuchsia
