# Copyright 2023 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
import argparse
from http.server import BaseHTTPRequestHandler, HTTPServer
import json
import os
import requests
import shutil
import subprocess
from typing import List
from urllib.parse import parse_qs, urlparse


class PubSub:

    def __init__(self):
        self.subscribers = {}

    def subscribe(self, topic, config):
        if topic not in self.subscribers:
            self.subscribers[topic] = []
        self.subscribers[topic].append(config)

    def publish(self, topic, path):
        for config in self.subscribers.get(topic, []):
            repo = os.path.expanduser(config['repo'])
            cmds = config['commands']
            dest_path = config['listen_to'][topic]
            publish_table = config.get('publish', {})

            if dest_path == None:
                dest_path = path
            if dest_path != path:
                shutil.copy(path, os.path.join(repo, dest_path))

            execute_commands_in_repo(repo, cmds, dest_path)
            # Publish downstream messages
            for topic, message in publish_table.items():
                pub.publish(topic, os.path.join(
                    repo, message))


pub = PubSub()


class PubSubHandler(BaseHTTPRequestHandler):

    def do_GET(self):
        url_parts = urlparse(self.path)
        query_params = parse_qs(url_parts.query)
        topic = query_params.get('event')[0]
        message = query_params.get('message')[0]
        self.send_response(200)
        self.end_headers()

        pub.publish(topic, message)


def execute_commands_in_repo(repo: str, commands: List[str], dest_path: str):
    print(f'Run in repo: {repo}')
    for command in commands:
        cmd = command.format(dest_path=dest_path).split()
        print(f'Execute cmd: {cmd}')
        result = subprocess.run(cmd, cwd=repo)


def register_config(config_path: str):
    if not config_path:
        return
    with open(config_path, 'r') as f:
        rule_table = json.load(f)

    for _, config in rule_table.items():
        listen_to = config['listen_to']
        # Register config for each listened events
        for topic in listen_to:
            pub.subscribe(topic, config)


def start_server(args: argparse.Namespace):
    # Parse configuration file
    register_config(args.config)

    # Start server
    server_address = ('localhost', 8000)
    httpd = HTTPServer(server_address, PubSubHandler)
    print('Starting server...')
    httpd.serve_forever()


def publish_event(args: argparse.Namespace):
    response = requests.get(
        f'http://localhost:8000/?event={args.event}&message={args.message}')
    print(response)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    subparser = parser.add_subparsers(help='available commands')

    # Define start command
    parser_start = subparser.add_parser(
        'start',
        help='Start a pubsub server that listen to the events',
    )
    parser_start.add_argument(
        '--config',
        help='A path to the config file.',
    )
    parser_start.set_defaults(func=start_server)

    # Define publish command
    parser_start = subparser.add_parser(
        'publish',
        help='Publish an event',
    )
    parser_start.add_argument(
        'event',
        help='Event name published.',
    )
    parser_start.add_argument(
        '--message',
        help='Addional message for the published event.',
        default='any',
    )
    parser_start.set_defaults(func=publish_event)

    return parser.parse_args()


def main():
    args = parse_arguments()
    args.func(args)


if __name__ == '__main__':
    main()
