#!/usr/bin/env python3.8
# Copyright 2019 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Fuchsia packet capture and display tool.

fx sniff captures the packets flowing in-and-out the Fuchsia target device
and displays the packets in a useful view. This is to run on the development
host.

fx sniff will automatically filter out fx-workflow related packets such as ssh,
package server, logs, zxdb, etc., so that the user can focus on the application
of interest. For those who need to debug the fx workflow itself,
the full command under use is also printed; one can easily modify to meet
their own needs.

[Typical usages]
$ fx sniff wlan                    # capture packets over WLAN interface,
                                   # and show their summaries
$ fx sniff --view hex eth          # capture packets over Ethernet interface,
                                   # and show their hexadecimal dump
$ fx sniff --view wireshark wlan   # capture packets over WLAN interface,
                                   # and start wireshark GUI for display
$ fx sniff --file myfile eth       # capture packets and store
                                   # at //out/myfile.pcapng
$ fx sniff -t 10 wlan              # capture for 10 sec
$ fx sniff --help                  # show all command line options
"""

import argparse
import fcntl
import os
import subprocess
import sys
import termios
import time

LINUX_BIN_WIRESHARK = u"wireshark"
LINUX_BIN_TSHARK = u"tshark"
TARGET_TMP_DIR = u"/tmp/"


def has_cmd(binary_name):
    return any(
        os.access(os.path.join(path, binary_name), os.X_OK)
        for path in os.environ["PATH"].split(os.pathsep))


def has_wireshark_env():
    """Test if wireshark GUI can run.

    Returns:
      (True, "") if wireshark can run in the environment.
      (False, Error_String) otherwise.
    """
    platform = os.uname()
    print(platform)
    if not platform:
        return False, u"Failed to get uname"
    if platform[0].lower() != u"linux":
        return False, u"Supported only on Linux"

    if not has_cmd(LINUX_BIN_WIRESHARK):
        return False, u"can\'t find %s" % LINUX_BIN_WIRESHARK

    # All look good.
    return True, u""


def run_cmd(cmd):
    """Run subprocess.run() safely and returns the stdout.

    Args:
      cmd: a command line string.

    Returns:
      The stdout outcome of the shell command.
    """
    result = subprocess.run(
        cmd.split(), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return result.stdout.decode()


def can_run_cmd(cmd):
    """Test if the environment can run cmd.

    Args:
      cmd: a command line string.

    Returns:
      True if the command can run without error.
      False otherwise.
    """
    try:
        subprocess.check_call(
            cmd.split(), stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        return True
    except subprocess.CalledProcessError:
        return False


def invoke_shell(cmd):
    """invoke_shell() uses shell=True to support the command
    that includes shell escape sequences.

    Args:
      cmd: command string, which may include escape sequence.
    """
    print(u"Invoke shell:" + cmd)
    p = subprocess.Popen(cmd, shell=True)
    p.wait()


def get_interface_names():
    """Get all network interface names of the target except for the loopback.

    Returns:
      A list of interface names.
    """
    result = run_cmd(u"fx shell net if list")

    names = []
    for line in result.split(u"\n"):
        if u"name" not in line:
            # Look for name field only.
            continue
        parsed = line.split()
        if len(parsed) != 2:
            continue

        name = parsed[1].strip()

        if name == u"lo":
            # sniffing on loopback interface is not supported
            continue
        names.append(name)
    return names


def get_interface_filepath(interface_name):
    """Get the filepaths for all the interfaces on the target.

    Args:
      interface_name: interface name.

    Returns:
      A list of the interface file paths.
    """
    result = run_cmd(u"fx shell net if list %s" % interface_name)

    filepaths = []
    for line in result.split(u"\n"):
        if u"filepath" not in line:
            continue
        parsed = line.split()
        if len(parsed) != 2:
            continue

        filepath = parsed[1].strip()

        if filepath.lower() == u"[none]":
            # sniffing on loopback interface is not supported
            continue
        filepaths.append(filepath)
    return filepaths


def has_fuzzy_name(name_under_test, names):
    if not name_under_test:
        return False
    for n in names:
        if name_under_test in n:
            return True
    return False


def build_cmd(args):
    """Build cmd line for sniffing and displaying.

    Args:
      args: command line arguments.

    Returns:
      cmd string.
    """
    fx_workflow_filter = (
        u"not ( "
        u"port ssh or dst port 8083 or dst port 2345 or port 1900 "
        u"or ip6 dst port 33330-33341 or ip6 dst port 33337-33338"
        u" )")

    # Pay special attention to the escape sequence
    # This command goes through the host shell, and ssh shell.
    cmd_prefix = u"fx shell sh -c '\"netdump -t %s -f \\\"%s\\\" " % (
        args.timeout, fx_workflow_filter)
    cmd_suffix = u" %s\"'" % args.interface_filepath
    cmd_options = u""

    # Build more options
    if args.file:
        full_path = u"%s%s" % (TARGET_TMP_DIR, args.file)
        cmd_options += u"-w %s " % full_path

    if args.view == u"summary":
        cmd_options += u""  # Default behavior
    elif args.view == u"hex":
        cmd_options += u"--hexdump"
    elif args.view == u"wireshark":
        (result, err_str) = has_wireshark_env()
        if not result:
            msg = (
                u"Does not have a working wireshark envirionment. "
                u"Note it requires graphical environment "
                u"such as X Display: %s" % err_str)
            print(msg)
            return
        cmd_options += u"--pcapdump"
        cmd_suffix += u" | wireshark -k -i -"

    cmd = cmd_prefix + cmd_options + cmd_suffix
    return cmd


def get_keystroke_unblocking():
    """Returns a keystroke in a non-blocking way.
    """

    fd = sys.stdin.fileno()

    attr_org = termios.tcgetattr(fd)
    flags_org = fcntl.fcntl(fd, fcntl.F_GETFL)

    attr_new = attr_org[::]
    attr_new[3] = attr_new[3] & ~termios.ICANON & ~termios.ECHO
    flags_new = flags_org | os.O_NONBLOCK

    try:
        termios.tcsetattr(fd, termios.TCSANOW, attr_new)
        fcntl.fcntl(fd, fcntl.F_SETFL, flags_new)
        return sys.stdin.read(1)
    finally:
        termios.tcsetattr(fd, termios.TCSAFLUSH, attr_org)
        fcntl.fcntl(fd, fcntl.F_SETFL, flags_org)


def do_sniff(cmd):
    """Run user-interruptible sniffer.

    Args:
      cmd: command string, which may include escape sequence.
    """
    print("Run: {}".format(cmd))
    p = subprocess.Popen(cmd, shell=True)
    while p.poll() is None:
        time.sleep(0.07)  # To tame the CPU cycle consumption
        user_key = get_keystroke_unblocking()
        if user_key in ["q", "c", "Q", "C"]:
            print(" ... forced stop by user ({})".format(user_key))
            run_cmd("fx shell killall netdump")
            p.terminate()


def move_out_dumpfile(filename):
    """Move the PCAPNG dump file from the target device to the host device.

    Args:
      filename: filename stored in the target. Empty string if no file was stored.
    """
    if not filename:
        return

    full_path = u"%s%s" % (TARGET_TMP_DIR, filename)
    cmd = u"cd %s" % os.environ[u"FUCHSIA_OUT_DIR"]
    cmd += u"; fx scp \"[$(fx get-device-addr)]:%s\" ." % full_path
    cmd += u"; fx shell rm -rf %s" % full_path
    invoke_shell(cmd)


def is_target_ready():
    """Tests if the target Fuchsia device is ready to capture packets.

    Returns:
      True if the target is ready. False otherwise.
    """
    if not can_run_cmd("fx shell exit"):
        print("failed to run: the target device unreachable by 'fx shell'")
        return False
    if not can_run_cmd("fx shell which netdump"):
        msg = (
            "failed to run: the target does not have 'netdump'. "
            "Build with '--with-base //src/connectivity/network/netdump' "
            "and reload the target")
        print(msg)
        return False
    return True


def main():
    if not is_target_ready():
        sys.exit(1)

    iface_names = get_interface_names()

    parser = argparse.ArgumentParser(
        description=u"Capture packets on the target, Display on the host")

    parser.add_argument(
        u"interface_name",
        nargs=u"?",
        default=u"",
        help=u"Choose one interface name from: %s" %
        (" ".join(i for i in iface_names)))
    parser.add_argument(
        u"-t", u"--timeout", default=30, help=u"Time duration to sniff")
    parser.add_argument(
        u"--view",
        nargs=u"?",
        choices=[u"wireshark", u"hex", u"summary"],
        default=u"summary",
        const=u"summary",
        help=u"Wireshark requires X Display GUI environment.")
    parser.add_argument(
        u"--file",
        type=str,
        default=u"",
        help=u"Store PCAPNG file in //out directory. May use with --view option"
    )
    parser.add_argument(
        u"--iface_filepath",
        dest=u"iface_filepath",
        default=u"",
        help=u"Specify the interface filepath directly. For advanced users.")

    args = parser.parse_args()

    # Sanitize the file name
    if args.file:
        if not args.file.endswith(u".pcapng"):
            args.file = args.file + ".pcapng"

    if not has_fuzzy_name(args.interface_name, iface_names):
        print(
            u"Choose one interface name from: %s" %
            " ".join(i for i in iface_names))
        sys.exit(1)

    iface_file_paths = get_interface_filepath(args.interface_name)
    if len(iface_file_paths) != 1:
        msg = u"Querying interface name |%s| yielded non-unique result: %s" % (
            args.interface_name, u" ".join(f for f in iface_file_paths))
        print(msg)
        sys.exit(1)

    args.interface_filepath = iface_file_paths[0]
    do_sniff(build_cmd(args))
    print(u"\nEnd of fx sniff")

    move_out_dumpfile(args.file)
    sys.exit(0)


if __name__ == u"__main__":
    sys.exit(main())
