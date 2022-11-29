# Driver Manager

The Driver Manager is responsible for enumerating, loading, and managing the life cycle of device
drivers. Driver Manager also vends the /dev directory to the rest of the system so that others
can access drivers.

## Building and Running

Driver Manager is built and run in every product. It is launched on startup.

If you're looking for running an isolated Driver Manager for testing, please see
[isolated_devmgr](/src/lib/isolated_devmgr/README.md).

## Kernel Commandline Options

The behavior of Driver Manager can also be changed by several kernel commandline options.
Please look at the list of [kernel commandline options](/docs/reference/kernel/kernel_cmdline.md)
and look for the options that start with `devmgr.*`
