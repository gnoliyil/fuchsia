# A11y Demo

## Running the demo
* Follow the instructions for [`tiles-session`](/src/ui/bin/tiles-session/README.md) in order to use tiles-session.
* Include `--with=//src/ui/a11y/bin/demo:a11y-demo` in your `fx set`.
* Compile and pave the image
* Run :
  $ ffx config set setui true // only need to run once
  $ ffx setui accessibility set --screen_reader true
  $ ffx session add fuchsia-pkg://fuchsia.com/a11y-demo#meta/a11y-demo.cm
