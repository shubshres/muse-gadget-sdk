#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
# SPDX-License-Identifier: Apache-2.0
"""Upload a model API key over USB without echo or shell-history exposure.

Examples:
    python3 tools/muse/model_key.py gemini --select
    python3 tools/muse/model_key.py openai --model gpt-4.1-mini --select
    python3 tools/muse/model_key.py anthropic --clear
    python3 tools/muse/model_key.py muse
"""
import argparse
import getpass
import re
import sys
import time

from chat import Board, BoardError, pick_port


def send_setting(board, command):
    board.drain(0.1)
    board.write_line(command)
    deadline = time.monotonic() + 5
    while (line := board.read_line(deadline)) is not None:
        if line.strip() == "@model ok":
            return
        if line.strip() == "@model error":
            raise BoardError("The board rejected the setting or could not save it.")
    raise BoardError("No save confirmation. Install firmware with direct model support.")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("provider", choices=("muse", "gemini", "openai", "anthropic"))
    parser.add_argument("--port", help="USB serial port")
    parser.add_argument("--model", help="Provider's API model ID")
    parser.add_argument("--select", action="store_true", help="Use this provider after saving its key")
    parser.add_argument("--clear", action="store_true", help="Remove this provider's stored key")
    args = parser.parse_args()
    if args.provider == "muse" and (args.model or args.clear):
        parser.error("Muse uses its existing pairing/token settings.")
    if args.model and not re.fullmatch(r"[A-Za-z0-9_.-]{1,95}", args.model):
        parser.error("Model ID must be 1–95 letters, digits, dots, underscores or hyphens.")
    if args.clear and args.select:
        parser.error("Choose --clear or --select.")
    try:
        with Board(args.port or pick_port()) as board:
            status = board.status()
            if not status or not status.get("chat"):
                raise BoardError("The board needs firmware with USB chat and direct model support.")
            if args.provider == "muse":
                send_setting(board, "model.provider=muse")
                print("Muse selected.")
                return 0
            key = "" if args.clear else getpass.getpass(f"{args.provider} API key (hidden): ")
            if not args.clear and (not 1 <= len(key) <= 511 or any(not 33 <= ord(c) <= 126 for c in key)):
                raise BoardError("Key must be 1–511 printable ASCII characters without spaces.")
            send_setting(board, f"model.{args.provider}.key=" + key)
            key = None
            print(f"{args.provider} API key {'removed' if args.clear else 'saved'} on the board.")
            if args.model:
                send_setting(board, f"model.{args.provider}.id={args.model}")
                print("Model ID saved.")
            if args.select:
                send_setting(board, "model.provider=" + args.provider)
                print(f"{args.provider} selected.")
            return 0
    except (BoardError, OSError, EOFError) as exc:
        print(f"model_key: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
