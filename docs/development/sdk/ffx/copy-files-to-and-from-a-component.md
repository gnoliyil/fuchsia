# Copy files to and from a component

Use `ffx component copy` to copy files to and from Fuchsia components on a device.

Note: If you need to copy to isolated storage that doesn't currently exist, see [Copy to an isolated storage](#copy-to-non-existent-storage).

## Concepts

Every Fuchsia component has a [namespace][namespace]. A Fuchsia component can
use persistent storage via [storage capabilities][storage-capabilities]. All
the examples in this guide select the `data` directory, which provides
persistent storage on the device.

Before you upload files to (or download files from) a Fuchsia component,
you need to identify the [absolute moniker][absolute-moniker] of your
target component on the device. To find a component's absolute moniker,
use the [`ffx component show`][ffx-component-show] command,
for example:

``` none {:.devsite-disable-click-to-copy}
$ ffx component show stash_secure
               Moniker: /core/stash_secure
                   URL: fuchsia-pkg://fuchsia.com/stash#meta/stash_secure.cm
                  Type: CML static component
       Component State: Resolved
           Instance ID: c1a6d0aebbf7c092c53e8e696636af8ec0629ff39b7f2e548430b0034d809da4
           ...
```

The example above shows the moniker of the `stash_secure` component is
`/core/stash_secure`.

Once you identify your target component's moniker, use this
moniker (as a parameter to the `ffx component copy`) to access
the component's namespace on the device. For the examples below, use
`/core/stash_secure`.

## Download a file from the device {:#download-a-file-from-the-device}

To download a file from a Fuchsia device to your host machine, run the
following command:

```
ffx component copy "<MONIKER>::<PATH_TO_FILE>" "<DESTINATION>"
```

Replace the following:

<table class="responsive">
   <tr>
      <th>Argument</th>
      <th>Value</th>
   </tr>
   <tr>
      <td>MONIKER</td>
      <td>The absolute moniker of your target component.
         For example, <code>/core/stash_secure</code>.
      </td>
   </tr>
   <tr>
      <td>PATH_TO_FILE</td>
      <td>The path and filename on the target component where you want
         to save the file. For example, <br> <code>/data/stash_secure.store</code>.
      </td>
   </tr>
   <tr>
      <td>DESTINATION</td>
      <td>The path to a local directory where you want to save the file.</td>
   </tr>
</table>


The following downloads `stash_secure.store` from the target
component to the host machine:

``` none {:.devsite-disable-click-to-copy}
$ ffx component copy /core/stash_secure::/data/stash_secure.store ./stash_secure.store
```

## Upload a file to the device {:#upload-a-file-to-the-device}

To setup your environment to use the example below, please run the following commands:

``` none {:.devsite-disable-click-to-copy}
$ cd $FUCHSIA_DIR
$ LOCAL_RESOURCE_DIR=src/developer/ffx/plugins/component/copy/test-driver/resources
```

To upload a file from your host machine to a Fuchsia device, run the
following command:

```
ffx component copy "<SOURCE>" "<MONIKER>::<PATH_TO_FILE>"
```

Replace the following:

<table class="responsive">
   <tr>
      <th>Argument</th>
      <th>Value</th>
   </tr>
   <tr>
      <td>SOURCE</td>
      <td>The path to a file you want to copy to the device.</td>
   </tr>
   <tr>
      <td>MONIKER</td>
      <td>The absolute moniker of your target component. For example,
         <code>/core/stash_secure</code>.
      </td>
   </tr>
   <tr>
      <td>PATH_TO_FILE</td>
      <td>The path and filename on the target component where you want
         to save the file. For example, <br> <code>/data/foo.txt</code>.
      </td>
   </tr>
</table>

The following uploads `foo.txt` from the host machine to the
target component running on the device:

``` none {:.devsite-disable-click-to-copy}
$ ffx component copy $LOCAL_RESOURCE_DIR/foo.txt /core/stash_secure::/data/foo.txt
```

## Upload a file to the device from another device {:#copy-device-to-device}

To upload a file from a Fuchsia device to another Fuchsia device, run the
following command:

```
ffx component copy "<MONIKER>::<PATH_TO_FILE>" "<MONIKER>::<PATH_TO_FILE>"
```

Replace the following:

<table class="responsive">
   <tr>
      <th>Argument</th>
      <th>Value</th>
   </tr>
   <tr>
      <td>SOURCE</td>
      <td>The path to a file you want to copy to the device.</td>
   </tr>
   <tr>
      <td>MONIKER</td>
      <td>The absolute moniker of your target component. For example,
         <code>/core/stash_secure</code> for the first moniker, and
         <code>/core/feedback</code> as the second moniker.
      </td>
   </tr>
   <tr>
      <td>PATH_TO_FILE</td>
      <td>The path and filename on the target component where you want
         to save the file. For example, <br> <code>/data/stash_secure.store</code>
         as both paths.
      </td>
   </tr>
</table>

The following snippet uploads `stash_secure.store` from the component `/core/stash_secure` to the target component `/core/feedback`.

``` none {:.devsite-disable-click-to-copy}
$ ffx component copy /core/stash_secure::/data/stash_secure.store /core/feedback::/data/stash_secure.store
```

## Wildcard Support and Multiple Files

To setup your environment to use the example below, please run the following commands:

``` none {:.devsite-disable-click-to-copy}
$ cd $FUCHSIA_DIR
$ LOCAL_RESOURCE_DIR=src/developer/ffx/plugins/component/copy/test-driver/resources
```

`ffx component copy` supports the use of the wildcard `*` to copy multiple files at once. Host paths
are expanded by the shell while remote paths are expanded by `ffx component copy`.

Note: [Z shell][zsh] and [fish][fish] both run into issues when expanding `*` for remote paths. This is fixed
by wrapping `*` with a `''`. Therefore we want to replace `*` with `'*'`. This issue does not
occur on bash.

Upload multiple files to device:

```
ffx component copy "<SOURCE_DIRECTORY>/*" "<MONIKER>::<PATH_TO_DIRECTORY>"
```

Upload multiple files from device:

```
ffx component copy "<MONIKER>::<PATH_TO_DIRECTORY>/*" "<SOURCE_DIRECTORY>"
```

You can also pass in multiple paths manually with any source path type. As an example:

```
ffx component copy "<MONIKER>::<PATH_TO_DIRECTORY>/*" "<SOURCE>" "<MONIKER>::<PATH_TO_DIRECTORY>"
```

For the desired command, replace the following:

<table class="responsive">
   <tr>
      <th>Argument</th>
      <th>Value</th>
   </tr>
   <tr>
      <td>SOURCE_DIRECTORY</td>
      <td>A path to your desired directory. For example,
         <code>$LOCAL_RESOURCE_DIR</code>.
      </td>
   </tr>
   <tr>
      <td>MONIKER</td>
      <td>The absolute moniker of your target component.  For example,
         <code>/core/feedback</code> for the first moniker, and
         <code>/core/stash_secure</code> as the second moniker.
      </td>
   </tr>
   <tr>
      <td>PATH_TO_DIRECTORY</td>
      <td>The directory of the target component where you like to expand a wildcard.
         For example, <br> <code>/data</code>.
      </td>
   </tr>
   <tr>
      <td>PATH_TO_FILE</td>
      <td>The path of a filename on a component. For example, <br> <code>/data/device_id.txt</code>.
      </td>
   </tr>
   <tr>
      <td>SOURCE</td>
      <td>The path to a file you want to copy to the device.
         For example, <code>$LOCAL_RESOURCE_DIR/flower.jpeg</code>
      </td>
   </tr>
</table>

The following snippet uploads all files in the `data` directory from the component `/core/feedback/` to the `data`
directory of the target component `/core/stash_secure`.

``` none {:.devsite-disable-click-to-copy}
$ ffx component copy /core/feedback::/data/* /core/stash_secure::/data/
```

The following snippet uploads all files in `$LOCAL_RESOURCE_DIR` to the `data` directory of the
target component `/core/stash_secure`.

``` none {:.devsite-disable-click-to-copy}
$ ffx component copy $LOCAL_RESOURCE_DIR/* /core/stash_secure::/data/
```

The following snippet uploads all files in the `data` directory from the component `/core/feedback/`, and the host file
`$LOCAL_RESOURCE_DIR/flower.jpeg` to the target component `/core/stash_secure`.

``` none {:.devsite-disable-click-to-copy}
$ ffx component copy /core/feedback::/data/* $LOCAL_RESOURCE_DIR/flower.jpeg /core/stash_secure::/data/
```

## Copying to an isolated storage that currently does not exist {:#copy-to-non-existent-storage}

To setup your environment to use the example below, please run the following commands:

``` none {:.devsite-disable-click-to-copy}
$ cd $FUCHSIA_DIR
$ LOCAL_RESOURCE_DIR=src/developer/ffx/plugins/component/copy/test-driver/resources
```

If you're trying to copy to an isolated storage that doesn't currently exist,
you need to use <br> `ffx component storage copy`. Instead of an absolute
moniker, this command uses a component's [instance id][component-id-index].<br>
By default, `ffx component storage` connects to the persistent
isolated storage `data`.

Upload a file to device:

```
ffx component storage copy "<SOURCE>" "<INSTANCE_ID>::<PATH_TO_FILE>"
```

Download a file from device:

```
ffx component storage copy "<INSTANCE_ID>::<PATH_TO_FILE>" "<DESTINATION>"
```

For the desired command, replace the following:

<table class="responsive">
   <tr>
      <th>Argument</th>
      <th>Value</th>
   </tr>
   <tr>
      <td>INSTANCE_ID</td>
      <td>An instance ID of a component to be created later. For example,
         <code>c1a6d0aebbf7c092c53e8e696636af8ec0629ff39b7f2e548430b0034d809da4</code>.
      </td>
   </tr>
   <tr>
      <td>PATH_TO_FILE</td>
      <td>The path and filename on the target component where you want
         to save the file. For example, <br> <code>/my-example.file</code>.
      </td>
   </tr>
   <tr>
      <td>SOURCE</td>
      <td>The path to a file you want to copy to the device.</td>
   </tr>
   <tr>
      <td>DESTINATION</td>
      <td>The path to a local directory where you want to save the
         file.
      </td>
   </tr>
</table>

The following snippet uploads `$LOCAL_RESOURCE_DIR/flower.jpeg` to the target component `/core/stash_secure`:

``` none {:.devsite-disable-click-to-copy}
$ ffx component storage copy $LOCAL_RESOURCE_DIR/flower.jpeg c1a6d0aebbf7c092c53e8e696636af8ec0629ff39b7f2e548430b0034d809da4::/flower.jpeg
```

The following snippet downloads `stash_secure.store` from the target componnet to `LOCAL_RESOURCE_DIR`:

``` none {:.devsite-disable-click-to-copy}
$ ffx component storage copy c1a6d0aebbf7c092c53e8e696636af8ec0629ff39b7f2e548430b0034d809da4::/stash_secure.store $LOCAL_RESOURCE_DIR/stash_secure.store
```

## List all directories and files {:#list-all-directories-and-files}

To list all directories and files in a component's storage, run
the following command:

```
ffx component storage list "<INSTANCE_ID>::<PATH>"
```


Replace the following:

<table class="responsive">
   <tr>
      <th>Argument</th>
      <th>Value</th>
   </tr>
   <tr>
      <td>INSTANCE_ID</td>
      <td>An instance ID of a component to be created later. For example,
         <code>c1a6d0aebbf7c092c53e8e696636af8ec0629ff39b7f2e548430b0034d809da4</code>.
      </td>
   </tr>
   <tr>
      <td>PATH</td>
      <td>A path on the target component.For example, <code>/</code> or <code>/my/path/</code>.
      </td>
   </tr>
</table>

The following snippet shows all directories and files in the root (`/`)
directory of the target component:

``` none {:.devsite-disable-click-to-copy}
$ ffx component storage list c1a6d0aebbf7c092c53e8e696636af8ec0629ff39b7f2e548430b0034d809da4::/
my-example.file
stash_secure.store
```

## Create a new directory {:#create-a-new-directory}

To create a new directory in a component's storage, run the
following command:

```
ffx component storage make-directory "<INSTANCE_ID>::<NEW_PATH>"
```

Replace the following:

<table class="responsive">
   <tr>
      <th>Argument</th>
      <th>Value</th>
   </tr>
   <tr>
      <td>INSTANCE_ID</td>
      <td>An instance ID of a component to be created later. For example,
         <code>c1a6d0aebbf7c092c53e8e696636af8ec0629ff39b7f2e548430b0034d809da4</code>.
      </td>
   </tr>
   <tr>
      <td>PATH</td>
      <td>The name of a new directory on the target component. For example, <code>/my-new-path</code>.
      </td>
   </tr>
</table>

The following snippet shows creates a new directory named `my-new-path` on
the target component:

``` none {:.devsite-disable-click-to-copy}
$ ffx component storage make-directory c1a6d0aebbf7c092c53e8e696636af8ec0629ff39b7f2e548430b0034d809da4::/my-new-path
```

<!-- Reference links -->
[storage-capabilities]: /docs/concepts/components/v2/capabilities/storage.md
[ffx-component-storage]: https://fuchsia.dev/reference/tools/sdk/ffx#storage
[ffx-component-show]: ./view-component-information.md#get-detailed-information-from-a-component
[component-id-index]: /docs/development/components/component_id_index.md
[absolute-moniker]: /docs/reference/components/moniker.md#absolute
[namespace]: /docs/concepts/process/namespaces.md
[zsh]: https://zsh.sourceforge.io/
[fish]: https://fishshell.com/