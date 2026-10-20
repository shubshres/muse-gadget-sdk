#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Store an Azure Speech key on the connected Waveshare 1.75C over USB.

    python3 tools/muse/azure_key.py
    python3 tools/muse/azure_key.py --port /dev/cu.usbmodem101

The key is prompted without terminal echo and is never placed in shell history.
"""

import argparse
import getpass
import sys
import time

from chat import Board, BoardError, pick_port


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="USB serial port, if more than one board is connected")
    args = parser.parse_args()
    try:
        with Board(args.port or pick_port()) as board:
            status = board.status()
            if not status or status.get("board") != "Waveshare ESP32-S3-Touch-AMOLED-1.75C":
                raise BoardError("The connected board did not identify itself as the Waveshare 1.75C.")
            key = getpass.getpass("Azure Speech key (hidden): ")
            if not 1 <= len(key) <= 127 or any(not 33 <= ord(c) <= 126 for c in key):
                raise BoardError("The key must be 1–127 printable ASCII characters with no spaces.")
            board.drain(0.1)
            board.write_line("azure.key=" + key)
            key = None
            deadline = time.monotonic() + 5
            while (line := board.read_line(deadline)) is not None:
                if "@azure.key ok" in line:
                    print("Azure Speech key saved on the board.")
                    return 0
                if "@azure.key error" in line:
                    raise BoardError("The board rejected the key or could not save it.")
            raise BoardError("The board did not confirm the key was saved.")
    except (BoardError, OSError) as exc:
        print(f"azure_key: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
