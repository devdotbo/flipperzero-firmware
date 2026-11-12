#!/usr/bin/env python3
"""Remote control and debugging of the Govee H6006 Flipper app over the USB serial CLI.

Commands go in with `loader signal <signal> <hex arg>`; results come
back through /ext/apps_data/govee_h6006/remote_status.txt, which the app writes
when it receives signal 100 and which this tool reads with `storage read`.

Usage:
  govee_remote.py [--port PORT] [--timeout S] COMMAND [ARGS]

Commands:
  open                 start the app (refuses if another app is running)
  close                close the app (only if it is the running app; fails if it keeps running)
  status               dump and print the app status
  on | off             power all targeted lamps on or off
  bri N                brightness 1..100
  ct KELVIN            colour temperature 2700..6500
  rgb RRGGBB           RGB colour, hex
  scan                 rescan for lamps; the app starts the scan once the current lamp
                       updates have finished and ignores the request while a scan runs
  apply                send the full state again to all targeted lamps
  forget N             remove lamp N (1-based, as listed by status)
  target N             0 = all lamps, N = only lamp N
  lightshow on|off     lightshow (exactly one lamp must be targeted)
  events [--clear]     print the engine event ring; --clear empties it afterwards
                       (--clear may also go before the command)
  raw SIGNAL [HEXARG]  send one custom signal (100..110) after the app check
  selftest             offline test of the status parser and the wait logic

Exit codes: 0 ok, 1 a targeted lamp was not submitted, 2 usage, refused, or
rejected by the app (target, forget, lightshow, close, a scan the Bluetooth
stack refused), 3 deadline passed, 4 serial or CLI failure.

Needs pyserial for device commands (imported lazily, so `selftest` runs without it).
"""

from __future__ import annotations

import argparse
import glob
import os
import secrets
import sys
import time

APP_IDS = ("govee_h6006", "Govee H6006")
DEFAULT_FAP = "/ext/apps/Bluetooth/govee_h6006.fap"
STATUS_PATH = "/ext/apps_data/govee_h6006/remote_status.txt"
BAUD = 230400
PROMPT = b">:"
DUMP_GRACE_S = 3.0
# ask for a new dump again when no answer to the request appeared within this time
DUMP_REREQUEST_S = 1.0
# how long close waits for the loader to report no running app; above the
# app's own teardown bound, so a slow but healthy exit still counts as closed
CLOSE_WAIT_S = 8.0
# engine states in which a terminal lamp outcome can still change (cancel or close pending)
SETTLING_STATES = ("cancelling", "disconnecting")

SIG_STATUS = 100
SIG_POWER = 101
SIG_BRIGHTNESS = 102
SIG_KELVIN = 103
SIG_RGB = 104
SIG_SCAN = 105
SIG_APPLY = 106
SIG_FORGET = 107
SIG_TARGET = 108
SIG_LIGHTSHOW = 109
SIG_CLEAR_EVENTS = 110

FAILURE_OUTCOMES = {
    "connect_failed",
    "connect_timeout",
    "cancel_failed",
    "discovery_failed",
    "write_rejected",
    "link_lost",
    "aborted",
}
SUCCESS_OUTCOMES = {"submitted", "unchanged"}

EXIT_OK, EXIT_LAMP_FAILED, EXIT_USAGE, EXIT_TIMEOUT, EXIT_IO = 0, 1, 2, 3, 4


class ToolError(Exception):
    def __init__(self, message: str, code: int = EXIT_USAGE):
        super().__init__(message)
        self.code = code


# --------------------------------------------------------------------------
# Status file parser (format documented in ../core/govee_status.h)
# --------------------------------------------------------------------------


class Status:
    def __init__(self) -> None:
        self.kv: dict[str, str] = {}
        self.lamps: list[dict[str, str]] = []
        self.events: list[dict[str, str]] = []
        self.complete = False
        self.seq: int | None = None

    def get_int(self, key: str, default: int = -1) -> int:
        try:
            return int(self.kv[key])
        except (KeyError, ValueError):
            return default

    @property
    def rev(self) -> int:
        return self.get_int("desired.rev")

    @property
    def session(self) -> int:
        """Run marker of the app that wrote the file; 0 for files without it."""
        return self.get_int("session", 0)

    @property
    def engine_state(self) -> str:
        return self.kv.get("engine.state", "unknown")


def parse_lamp_value(value: str) -> dict[str, str]:
    lamp: dict[str, str] = {}
    head, sep, name = value.partition(" name=")
    if not sep and value.startswith("name="):
        head, name = "", value[len("name=") :]
    lamp["name"] = name
    for token in head.split():
        k, eq, v = token.partition("=")
        if eq:
            lamp[k] = v
    return lamp


def parse_status(text: str) -> Status:
    """Parse the status text. Tolerates '\\r\\n', partial files and unknown keys."""
    st = Status()
    lines = [ln.rstrip("\r") for ln in text.replace("\r\n", "\n").split("\n")]
    last_nonempty = ""
    for line in lines:
        if not line:
            continue
        last_nonempty = line
        key, eq, value = line.partition("=")
        if not eq:
            continue
        if key.startswith("lamp.") and key[5:].isdigit():
            lamp = parse_lamp_value(value)
            lamp["index"] = key[5:]
            st.lamps.append(lamp)
        elif key.startswith("ev.") and key[3:].isdigit():
            parts = value.split()
            ev = {"seq": key[3:], "t_ms": parts[0] if parts else "", "name": parts[1] if len(parts) > 1 else ""}
            for token in parts[2:]:
                k, eq2, v = token.partition("=")
                if eq2:
                    ev[k] = v
            st.events.append(ev)
        else:
            st.kv[key] = value
    if "seq" in st.kv and st.kv["seq"].isdigit():
        st.seq = int(st.kv["seq"])
    end_key, _, end_value = last_nonempty.partition("=")
    st.complete = (
        st.seq is not None
        and end_key == "end"
        and end_value.isdigit()
        and int(end_value) == st.seq
        and len(st.lamps) == st.get_int("lamps", -2)
        and len(st.events) == st.get_int("events", -2)
    )
    return st


def lamp_terminal(lamp: dict[str, str], rev: int) -> tuple[bool, bool]:
    """(terminal, ok) for a lamp and the revision we wait for."""
    try:
        reached = int(lamp.get("rev", "0"))
        attempted = int(lamp.get("try", "0"))
    except ValueError:
        return False, False
    if rev <= 0:
        # revision 0 means nothing was ever submitted in this app run
        return True, False
    if reached >= rev:
        return True, True
    if attempted >= rev and lamp.get("out", "") in FAILURE_OUTCOMES:
        return True, False
    return False, False


def targeted_lamps(st: Status) -> list[dict[str, str]]:
    return [lp for lp in st.lamps if lp.get("targeted") == "1"]


def format_lamp(lamp: dict[str, str], verdict: str | None = None) -> str:
    out = verdict or lamp.get("out", "?")
    return (
        f"lamp {lamp.get('index', '?'):>2} {lamp.get('addr', '?')} {out:<16} "
        f"conn={lamp.get('conn_ms', '?')}ms disc={lamp.get('disc_ms', '?')}ms "
        f"write={lamp.get('write_ms', '?')}ms dis={lamp.get('dis_ms', '?')}ms "
        f"total={lamp.get('total_ms', '?')}ms writes={lamp.get('writes', '?')} "
        f"reused={lamp.get('reused', '?')} rev={lamp.get('rev', '?')} "
        f"targeted={lamp.get('targeted', '?')} name={lamp.get('name', '')}"
    )


def evaluate(st: Status, rev: int) -> tuple[bool, bool, list[str]]:
    """(all_terminal, all_ok, lines) for the targeted lamps of a status at revision rev."""
    lines = []
    all_terminal = True
    all_ok = True
    for lamp in targeted_lamps(st):
        terminal, ok = lamp_terminal(lamp, rev)
        if not terminal:
            all_terminal = False
            all_ok = False
            lines.append(format_lamp(lamp, "pending"))
        else:
            if not ok:
                all_ok = False
            lines.append(format_lamp(lamp))
    return all_terminal, all_ok, lines


# --------------------------------------------------------------------------
# Flipper CLI transport
# --------------------------------------------------------------------------


def find_port() -> str:
    ports = sorted(glob.glob("/dev/cu.usbmodemflip_*"))
    if not ports:
        raise ToolError("no Flipper found (/dev/cu.usbmodemflip_*); pass --port", EXIT_IO)
    if len(ports) > 1:
        raise ToolError("several Flippers found, pass --port: " + ", ".join(ports), EXIT_IO)
    return ports[0]


class FlipperCli:
    def __init__(self, port_name: str, command_timeout: float = 5.0) -> None:
        try:
            import serial  # lazy: selftest must run without pyserial
        except ImportError as exc:
            raise ToolError("pyserial is required for device commands (pip install pyserial)", EXIT_IO) from exc
        self.timeout = command_timeout
        try:
            self.port = serial.Serial(port_name, BAUD, timeout=0.2)
        except serial.SerialException as exc:  # type: ignore[attr-defined]
            raise ToolError(f"cannot open {port_name}: {exc}", EXIT_IO) from exc
        time.sleep(0.3)
        try:
            self.port.write(b"\r")
            self._read_until_prompt(3.0)
        except OSError as exc:  # pyserial's SerialException is an OSError
            raise ToolError(f"serial I/O failed: {exc}", EXIT_IO) from exc

    def close(self) -> None:
        try:
            self.port.close()
        except OSError:
            pass

    def _read_until_prompt(self, limit: float) -> bytes:
        buf = b""
        start = time.monotonic()
        while time.monotonic() - start < limit:
            chunk = self.port.read(4096)
            if chunk:
                buf += chunk
                if buf.rstrip().endswith(PROMPT):
                    return buf
        raise ToolError(f"CLI prompt not seen within {limit:.1f} s", EXIT_IO)

    def run(self, command: str) -> str:
        try:
            self.port.reset_input_buffer()
            self.port.write(command.encode() + b"\r")
            raw = self._read_until_prompt(self.timeout).decode("utf-8", "replace")
        except OSError as exc:  # pyserial's SerialException is an OSError
            raise ToolError(f"serial I/O failed: {exc}", EXIT_IO) from exc
        text = raw.replace("\r\n", "\n").replace("\r", "")
        lines = text.split("\n")
        # drop the echoed command and the trailing prompt
        if lines and lines[0].strip() == command.strip():
            lines = lines[1:]
        while lines and (not lines[-1].strip() or lines[-1].strip().endswith(">:")):
            lines.pop()
        return "\n".join(lines)


# --------------------------------------------------------------------------
# App operations
# --------------------------------------------------------------------------


class App:
    def __init__(self, cli, poll_interval: float = 0.5, sleep=time.sleep, clock=time.monotonic) -> None:
        self.cli = cli
        self.poll_interval = poll_interval
        self.sleep = sleep
        self.clock = clock
        self.last_seq = -1
        # request id sent as the arg of signal 100 and echoed by the app as req=
        self.req = secrets.randbits(32)
        # an answer to a request sent in this run had a req= line: the running app echoes
        # request ids. Never learned from a file already on the card before the request
        # (it can be left by another app build).
        self.echoes_req = False
        # request ids sent with signal 100 in this run
        self.sent_reqs: set[int] = set()
        # the file on the card before this call's request has a req= line. Only a hint: a
        # refused request is sent once more with its id before the arg-0 fallback
        self.req_hint = False
        self.refused_reqs: set[int] = set()
        # a request with an id was not accepted and the app has not shown a req= line yet:
        # status requests use arg 0 until a req= line appears (app build without req=)
        self.legacy_status = False

    def running_app(self) -> str | None:
        reply = self.cli.run("loader info")
        if "No application is running" in reply:
            return None
        start = reply.find('"')
        end = reply.find('"', start + 1)
        if start >= 0 and end > start:
            return reply[start + 1 : end]
        return reply.strip() or None

    def require_app(self) -> None:
        name = self.running_app()
        if name not in APP_IDS:
            raise ToolError(f"govee_h6006 is not the running app (running: {name or 'none'}); refusing to signal")

    def signal(self, signal: int, arg: int = 0) -> None:
        self.require_app()  # checked before every signal, not once per command
        reply = self.cli.run(f"loader signal {signal} {arg:x}")
        if "was handled" not in reply:
            raise ToolError(f"signal {signal} arg 0x{arg:x} not accepted by the app: {reply.strip()}")

    def read_status_file(self) -> Status | None:
        reply = self.cli.run(f"storage read {STATUS_PATH}")
        lines = reply.split("\n")
        if not lines or not lines[0].startswith("Size:"):
            return None
        return parse_status("\n".join(lines[1:]))

    def next_req(self) -> int:
        self.req = (self.req % 0xFFFFFFFF) + 1  # 1..0xFFFFFFFF, never 0 (0 = no request id)
        return self.req

    def request_status(self, req: int) -> None:
        """Send signal 100 with the request id. An app build without req= ignores a
        non-zero arg; while no answer in this run has shown a req= line, the tool then
        falls back to arg 0 and the sequence logic. An app that echoes request ids can also
        refuse a request (its inbox was full); the request is then sent again with the
        same id after DUMP_REREQUEST_S. Before the app has echoed an id in this run, a
        req= line in the file read before the request only delays the fallback: the
        refused id is sent once more, and a second refusal falls back to arg 0."""
        if not self.legacy_status:
            self.require_app()
            self.sent_reqs.add(req)
            reply = self.cli.run(f"loader signal {SIG_STATUS} {req:x}")
            if "was handled" in reply or self.echoes_req:
                return
            if self.req_hint and req not in self.refused_reqs:
                self.refused_reqs.add(req)
                return
            self.legacy_status = True
        self.signal(SIG_STATUS, 0)

    @staticmethod
    def new_by_sequence(st: Status, baseline: int | None, base_session: int | None, requests: int) -> bool:
        """Session and sequence rule: st was written after the baseline file, answering
        the latest of the requests sent in this call."""
        return baseline is None or st.session != base_session or st.seq >= baseline + requests

    def learn_echo(self, st: Status, baseline: int | None, base_session: int | None, requests: int) -> None:
        """Learn that the running app echoes request ids, only from an answer to a request
        sent in this run: a req= line with an id this run sent, or, when a status file was
        read before the request (baseline), a req= line in a file the session and sequence
        rule shows as written after this call's request. Without a baseline only an echoed
        id of this run counts. A file left on the card by another app build never counts."""
        if "req" not in st.kv:
            return
        own_id = st.get_int("req", 0) in self.sent_reqs
        if own_id or (baseline is not None and self.new_by_sequence(st, baseline, base_session, requests)):
            self.echoes_req = True
            self.legacy_status = False

    def answers(self, st: Status, req: int, baseline: int | None, base_session: int | None, requests: int) -> bool:
        if "req" in st.kv:
            # in the arg-0 fallback the app's answers carry req=0: judged by session and
            # sequence like a file without req=
            if self.legacy_status and st.get_int("req", 0) == 0:
                return self.new_by_sequence(st, baseline, base_session, requests)
            # otherwise a file with a req= line (also 0) is matched by id: only the dump
            # that echoes this call's request id; the app handles the inbox in order, so it
            # was written after every signal sent before the request
            return st.get_int("req", 0) == req
        return self.new_by_sequence(st, baseline, base_session, requests)

    def dump(self, deadline: float) -> Status:
        """Request a dump and return a complete status written for this request.

        With an app that echoes the request id (req=), only the answer carrying this
        call's id is accepted, so a dump requested before the caller's command (an earlier
        raw 100, or an unanswered request left over from a restart) is never taken. A
        file with a req= line is matched by id, also with req=0, except in the arg-0
        fallback, where a req=0 file is judged by session and sequence. The tool learns
        that the app echoes ids only from an answer to a request of this run (learn_echo),
        never from the file read before the request, which another app build may have left
        on the card; without such a baseline file, only an echo of an id sent in this run
        counts.
        An app build that never writes a req= line is matched by session and sequence
        number instead: both are read from the file on the card before signal 100. A dump
        from another session (the app was restarted; a file without a session line is
        session 0) is always new. In the same session the app answers requests in order,
        one number each, so with n requests sent the answer to the latest is baseline + n;
        a smaller number answers an earlier request and is skipped.
        Without an accepted answer after DUMP_REREQUEST_S the request is sent again.
        A dump requested before the deadline may finish up to DUMP_GRACE_S after it, so the
        final per-lamp report is still printed when the overall deadline passes."""
        prior = self.read_status_file()
        baseline = prior.seq if prior is not None else None
        base_session = prior.session if prior is not None else None
        self.req_hint = prior is not None and "req" in prior.kv
        req = self.next_req()
        self.request_status(req)
        requests = 1
        last_request = self.clock()
        hard_deadline = max(deadline, self.clock()) + DUMP_GRACE_S
        while True:
            st = self.read_status_file()
            if st is not None and st.complete and st.seq is not None:
                self.learn_echo(st, baseline, base_session, requests)
                if self.answers(st, req, baseline, base_session, requests):
                    self.last_seq = st.seq
                    return st
            if self.clock() >= hard_deadline:
                raise ToolError("no complete status file before the deadline", EXIT_TIMEOUT)
            if self.clock() - last_request >= DUMP_REREQUEST_S:
                self.request_status(req)
                requests += 1
                last_request = self.clock()
            self.sleep(0.2)

    def change_and_wait(self, signal: int, arg: int, timeout: float, out) -> int:
        self.require_app()
        deadline = self.clock() + timeout
        before = self.dump(deadline)
        self.signal(signal, arg)
        rev = None
        st = before
        while True:
            st = self.dump(deadline)
            if rev is None:
                if st.rev == before.rev and st.kv.get("scanning") == "1":
                    # a running scan defers settings and apply; the command is pending
                    if self.clock() >= deadline:
                        print(f"deadline of {timeout:.0f} s passed while a scan defers the command", file=out)
                        return EXIT_TIMEOUT
                    self.sleep(self.poll_interval)
                    continue
                rev = st.rev
                if rev <= 0:
                    print("nothing was submitted in this app run (rev 0)", file=out)
                elif rev == before.rev:
                    # same revision; the targeted lamps still decide the exit code
                    print(f"no change: desired state already set (rev {rev})", file=out)
            all_terminal, all_ok, lines = evaluate(st, rev)
            stuck = st.engine_state in ("faulted", "stopped")
            settling = st.engine_state in SETTLING_STATES
            if all_terminal and settling and not stuck and self.clock() < deadline:
                # a cancel or close is still running; its result may change an outcome
                self.sleep(self.poll_interval)
                continue
            if all_terminal or stuck or self.clock() >= deadline:
                print(
                    f"rev {rev} engine={st.engine_state} fault={st.kv.get('engine.fault', '?')} "
                    f"pass={st.kv.get('engine.pass', '?')}",
                    file=out,
                )
                for line in lines:
                    print(line, file=out)
                if not targeted_lamps(st):
                    print("no lamp is targeted", file=out)
                    return EXIT_LAMP_FAILED
                if all_terminal:
                    return EXIT_OK if all_ok else EXIT_LAMP_FAILED
                if stuck:
                    print(f"engine is {st.engine_state}; pending lamps will not be served", file=out)
                    return EXIT_LAMP_FAILED
                print(f"deadline of {timeout:.0f} s passed", file=out)
                return EXIT_TIMEOUT
            self.sleep(self.poll_interval)


def print_status(st: Status, out) -> None:
    for key in (
        "seq",
        "session",
        "app",
        "target",
        "scanning",
        "scan.refused",
        "lightshow",
        "registry.count",
        "registry.full",
        "registry.load",
        "registry.save",
        "inbox_overflow",
        "desired.rev",
        "desired.power",
        "desired.brightness",
        "desired.mode",
        "desired.kelvin",
        "desired.rgb",
        "engine.state",
        "engine.fault",
        "engine.pass",
        "engine.pending",
    ):
        if key in st.kv:
            print(f"{key}={st.kv[key]}", file=out)
    for lamp in st.lamps:
        print(format_lamp(lamp), file=out)


def print_events(st: Status, out) -> None:
    print(f"events={len(st.events)} total={st.kv.get('events.total', '?')}", file=out)
    for ev in st.events:
        print(
            f"#{ev.get('seq')} t={ev.get('t_ms')}ms {ev.get('name')} lamp={ev.get('lamp', '-')} arg={ev.get('arg', '')}",
            file=out,
        )


def parse_int(text: str, lo: int, hi: int, what: str, base: int = 10) -> int:
    try:
        value = int(text, base)
    except ValueError as exc:
        raise ToolError(f"{what} must be a number, got {text!r}") from exc
    if not lo <= value <= hi:
        raise ToolError(f"{what} must be {lo}..{hi}, got {value}")
    return value


def run_command(app: App, args, out) -> int:
    cmd = args.command
    rest = args.args
    timeout = args.timeout

    def need(n: int) -> None:
        if len(rest) != n:
            raise ToolError(f"{cmd} takes {n} argument(s)")

    if cmd == "open":
        name = app.running_app()
        if name in APP_IDS:
            print("already running", file=out)
            return EXIT_OK
        if name:
            raise ToolError(f"another app is running: {name}")
        print(app.cli.run(f"loader open {args.fap}"), file=out)
        return EXIT_OK if app.running_app() in APP_IDS else EXIT_USAGE
    if cmd == "close":
        app.require_app()
        reply = app.cli.run("loader close")
        print(reply, file=out)
        # "was closed" only means the exit request was accepted; success only
        # once the loader reports no running app
        until = app.clock() + CLOSE_WAIT_S
        while True:
            if app.running_app() is None:
                return EXIT_OK
            if app.clock() >= until:
                raise ToolError(f"close refused, the app is still running: {reply.strip()}")
            app.sleep(0.2)
    if cmd == "status":
        app.require_app()
        print_status(app.dump(app.clock() + timeout), out)
        return EXIT_OK
    if cmd in ("on", "off"):
        need(0)
        return app.change_and_wait(SIG_POWER, 1 if cmd == "on" else 0, timeout, out)
    if cmd == "bri":
        need(1)
        return app.change_and_wait(SIG_BRIGHTNESS, parse_int(rest[0], 1, 100, "brightness"), timeout, out)
    if cmd == "ct":
        need(1)
        return app.change_and_wait(SIG_KELVIN, parse_int(rest[0], 2700, 6500, "kelvin"), timeout, out)
    if cmd == "rgb":
        need(1)
        return app.change_and_wait(SIG_RGB, parse_int(rest[0].lstrip("#"), 0, 0xFFFFFF, "rgb", 16), timeout, out)
    if cmd == "apply":
        need(0)
        return app.change_and_wait(SIG_APPLY, 0, timeout, out)
    if cmd == "scan":
        need(0)
        app.require_app()
        deadline = app.clock() + timeout
        app.signal(SIG_SCAN)
        while True:
            app.sleep(1.0)
            st = app.dump(deadline)
            if st.kv.get("scanning") == "0" or app.clock() >= deadline:
                print_status(st, out)
                if st.kv.get("scanning") != "0":
                    return EXIT_TIMEOUT
                # files from an app build without this line count as not refused
                if st.kv.get("scan.refused") == "1":
                    print("scan refused: the Flipper's Bluetooth stack did not start the scan", file=out)
                    return EXIT_USAGE
                return EXIT_OK
    if cmd in ("forget", "target", "lightshow"):
        need(1)
        app.require_app()
        deadline = app.clock() + timeout
        mismatch = None
        if cmd == "forget":
            index = parse_int(rest[0], 1, 16, "lamp")
            before = app.dump(deadline)
            gone = next((lp.get("addr") for lp in before.lamps if lp.get("index") == str(index)), None)
            app.signal(SIG_FORGET, index)
            st = app.dump(deadline)
            count = st.get_int("registry.count")
            if gone is None or count != before.get_int("registry.count") - 1 or any(
                lp.get("addr") == gone for lp in st.lamps
            ):
                mismatch = f"lamp {index} was not removed (lamps: {count})"
        elif cmd == "target":
            target = parse_int(rest[0], 0, 16, "target")
            app.signal(SIG_TARGET, target)
            st = app.dump(deadline)
            if st.kv.get("target") != str(target):
                mismatch = f"target is {st.kv.get('target', '?')}, requested {target}"
        else:
            if rest[0] not in ("on", "off"):
                raise ToolError("lightshow takes on or off")
            want = "1" if rest[0] == "on" else "0"
            app.signal(SIG_LIGHTSHOW, int(want))
            st = app.dump(deadline)
            if st.kv.get("lightshow") != want:
                mismatch = f"lightshow is {st.kv.get('lightshow', '?')}, requested {want} (one lamp must be targeted)"
        print_status(st, out)
        # the app may reject a valid-looking request; compare the result with it
        if mismatch:
            print(f"rejected by the app: {mismatch}", file=out)
            return EXIT_USAGE
        return EXIT_OK
    if cmd == "events":
        app.require_app()
        print_events(app.dump(app.clock() + timeout), out)
        if args.clear:
            app.signal(SIG_CLEAR_EVENTS)
            print("event ring cleared", file=out)
        return EXIT_OK
    if cmd == "raw":
        if len(rest) not in (1, 2):
            raise ToolError("raw takes SIGNAL [HEXARG]")
        signal = parse_int(rest[0], 100, 110, "signal")
        arg = parse_int(rest[1], 0, 0xFFFFFFFF, "arg", 16) if len(rest) == 2 else 0
        app.require_app()
        app.signal(signal, arg)
        print(f"signal {signal} arg 0x{arg:x} handled", file=out)
        return EXIT_OK
    raise ToolError(f"unknown command {cmd}")


# --------------------------------------------------------------------------
# Self-test (offline)
# --------------------------------------------------------------------------


class FakeCli:
    """Simulates the Flipper CLI and the app's status dumps for the wait logic."""

    def __init__(self, sample: str, script) -> None:
        self.sample = sample
        self.script = script  # callable(dump_number, sample) -> status text
        self.app = "Govee H6006"
        self.dumps = 0
        self.visible = 0  # dump number whose file the card shows
        self.lag_reads = 0  # reads that still show the previous file after a request
        self.reads = 0
        self.signals: list[str] = []
        self.partial_next = False
        self.close_reply = "was closed"
        self.close_linger = 0  # "loader info" replies that still show the app after "was closed"
        self.closing = False
        self.after_signal = None  # callable(fake, signal) run after each signal
        self.fail_io = False
        self.legacy = False  # app build without req=: ignores a non-zero status arg
        self.status_args: list[int] = []  # arg of each handled status request
        self.ignored: list[str] = []
        self.req_override = None  # callable(dump_number, fake) -> req to show instead, or None
        self.reject_status = 0  # next status requests the app refuses whatever the arg (inbox full)
        self.no_req_dumps: set[int] = set()  # dump numbers whose file has no req= line
        # dump numbers whose file is shown as the script wrote it (left by another app build)
        self.verbatim_dumps: set[int] = set()
        self.absent_dumps: set[int] = set()  # dump numbers for which the card has no status file

    def stamp(self, text: str, dump_no: int) -> str:
        """Put the req= line the app writes for dump number dump_no (none for a legacy app)."""
        if dump_no in self.verbatim_dumps:
            return text
        lines = [ln for ln in text.split("\n") if not ln.startswith("req=")]
        if self.legacy or dump_no in self.no_req_dumps:
            return "\n".join(lines)
        req = self.status_args[dump_no - 1] if 0 < dump_no <= len(self.status_args) else 0
        if self.req_override:
            override = self.req_override(dump_no, self)
            if override is not None:
                req = override
        lines.insert(1, f"req={req}")
        return "\n".join(lines)

    def run(self, command: str) -> str:
        if self.fail_io:
            raise OSError(5, "Input/output error (device disconnected)")
        if command == "loader info":
            if self.closing and self.app:
                if self.close_linger > 0:
                    self.close_linger -= 1
                else:
                    self.app = None
            return f'Application "{self.app}" is running' if self.app else "No application is running"
        if command == "loader close":
            if self.close_reply == "was closed":
                self.closing = True
                return f'Application "{self.app}" was closed'
            return f'Application "{self.app}" has to be closed manually'
        if command.startswith("loader signal"):
            if self.reject_status > 0 and command.split()[2] == str(SIG_STATUS):
                self.reject_status -= 1
                self.ignored.append(command)
                return f"Signal {command.split()[2]} with argument 0x{command.split()[3]} was ignored"
            if self.legacy and command.split()[2] == str(SIG_STATUS) and int(command.split()[3], 16) != 0:
                self.ignored.append(command)
                return f"Signal {command.split()[2]} with argument 0x{command.split()[3]} was ignored"
            self.signals.append(command)
            if command.split()[2] == str(SIG_STATUS):
                self.dumps += 1
                self.status_args.append(int(command.split()[3], 16))
                self.partial_next = True
            if self.after_signal:
                self.after_signal(self, int(command.split()[2]))
            return f"Signal {command.split()[2]} with argument 0x{command.split()[3]} was handled"
        if command.startswith("storage read"):
            self.reads += 1
            if self.lag_reads > 0 and self.dumps > self.visible:
                self.lag_reads -= 1
                if self.visible in self.absent_dumps:
                    return "Storage error: file/dir not exist\r\n"
                text = self.stamp(self.script(self.visible, self.sample), self.visible)
                return f"Size: {len(text)}\n" + text.replace("\n", "\r\n")
            self.visible = self.dumps
            if self.dumps in self.absent_dumps:
                return "Storage error: file/dir not exist\r\n"
            text = self.stamp(self.script(self.dumps, self.sample), self.dumps)
            if self.partial_next:
                # first read after a dump request sees a half-written file
                self.partial_next = False
                text = text[: len(text) // 2]
            return f"Size: {len(text)}\n" + text.replace("\n", "\r\n")
        raise AssertionError(command)


def rewrite(
    sample: str, seq: int, rev: int, lamp_fields: dict[int, dict[str, str]], kv: dict[str, str] | None = None
) -> str:
    out = []
    for line in sample.split("\n"):
        key = line.partition("=")[0]
        if kv and key in kv:
            line = f"{key}={kv[key]}"
        elif line.startswith("seq="):
            line = f"seq={seq}"
        elif line.startswith("end="):
            line = f"end={seq}"
        elif line.startswith("desired.rev="):
            line = f"desired.rev={rev}"
        elif line.startswith("lamp."):
            idx = int(line[5 : line.index("=")])
            if idx in lamp_fields:
                value = line[line.index("=") + 1 :]
                head, _, name = value.partition(" name=")
                tokens = []
                for tok in head.split():
                    k, _, v = tok.partition("=")
                    tokens.append(f"{k}={lamp_fields[idx].get(k, v)}")
                line = f"lamp.{idx}=" + " ".join(tokens) + " name=" + name
        out.append(line)
    return "\n".join(out)


def selftest(fixtures: str) -> int:
    import io

    failures = 0
    checks = 0

    def check(cond: bool, what: str) -> None:
        nonlocal failures, checks
        checks += 1
        if not cond:
            failures += 1
            print(f"  FAIL: {what}")

    sample_path = os.path.join(fixtures, "remote_status_sample.txt")
    partial_path = os.path.join(fixtures, "remote_status_partial.txt")
    with open(sample_path, encoding="ascii") as f:
        sample = f.read()
    with open(partial_path, encoding="ascii") as f:
        partial = f.read()

    st = parse_status(sample)
    check(st.complete, "sample is complete")
    check(st.seq == 7, "sample seq is 7")
    check(st.session == 1592594996, "sample session parsed")
    legacy = "\n".join(ln for ln in sample.split("\n") if not ln.startswith("session="))
    check(parse_status(legacy).complete and parse_status(legacy).session == 0, "file without session is session 0")
    check(st.rev == 1, "sample desired.rev is 1")
    check(st.engine_state == "idle", "engine state idle")
    check(st.kv.get("desired.brightness") == "80", "brightness 80")
    check(len(st.lamps) == 3, "three lamps")
    check(st.lamps[1].get("name") == "Hall = lamp 2", "name with spaces and '=' survives")
    check(st.lamps[1].get("out") == "connect_timeout", "lamp 2 connect_timeout")
    check(st.lamps[1].get("conn_ms") == "3000", "lamp 2 conn_ms 3000")
    check(st.lamps[0].get("out") == "submitted" and st.lamps[0].get("writes") == "3", "lamp 1 submitted, 3 writes")
    check(len(st.events) == st.get_int("events"), "event count matches")
    check(any(ev.get("name") == "connect_timeout" and ev.get("lamp") == "2" for ev in st.events), "event parsed")
    all_terminal, all_ok, lines = evaluate(st, 1)
    check(all_terminal and not all_ok and len(lines) == 3, "evaluate: terminal, one failure")

    # partial files are never complete
    check(not parse_status(partial).complete, "partial fixture is incomplete")
    check(not parse_status(sample[: len(sample) - 3]).complete, "cut end line is incomplete")
    check(not parse_status(sample.replace("end=7", "end=6")).complete, "mismatched end is incomplete")
    check(parse_status(sample.replace("\n", "\r\n")).complete, "CRLF tolerated")
    dropped = "\n".join(ln for ln in sample.split("\n") if not ln.startswith("lamp.2="))
    check(not parse_status(dropped).complete, "missing lamp line is incomplete")

    # wait logic against a fake CLI: lamp 2 fails, lamps 1 and 3 get rev 2 after two dumps
    def script(dumps: int, base: str) -> str:
        if dumps <= 1:
            return rewrite(base, 10 + dumps, 1, {})
        if dumps == 2:
            return rewrite(base, 10 + dumps, 2, {1: {"try": "2", "out": "none"}})
        return rewrite(
            base,
            10 + dumps,
            2,
            {1: {"rev": "2", "try": "2"}, 2: {"try": "2", "out": "connect_timeout"}, 3: {"rev": "2", "try": "2"}},
        )

    fake = FakeCli(sample, script)
    clock = [0.0]

    def fake_sleep(s: float) -> None:
        clock[0] += s

    app = App(fake, poll_interval=0.5, sleep=fake_sleep, clock=lambda: clock[0])
    out = io.StringIO()
    code = app.change_and_wait(SIG_BRIGHTNESS, 0x50, 30.0, out)
    text = out.getvalue()
    check(code == EXIT_LAMP_FAILED, f"wait exit code 1 on a failed lamp (got {code})")
    check("lamp  2" in text and "connect_timeout" in text, "failed lamp reported")
    check(text.count("submitted") == 2, "two lamps submitted")
    check("loader signal 102 50" in fake.signals, "brightness 80 sent as hex 50")
    check(fake.reads > fake.dumps, "partial files were re-read")

    # all lamps succeed
    def script_ok(dumps: int, base: str) -> str:
        if dumps <= 1:
            return rewrite(base, 20 + dumps, 1, {})
        return rewrite(base, 20 + dumps, 2, {i: {"rev": "2", "try": "2", "out": "submitted"} for i in (1, 2, 3)})

    app = App(FakeCli(sample, script_ok), sleep=fake_sleep, clock=lambda: clock[0])
    out = io.StringIO()
    check(app.change_and_wait(SIG_POWER, 1, 30.0, out) == EXIT_OK, "all submitted gives exit 0")

    # no lamp ever finishes: deadline
    def script_stuck(dumps: int, base: str) -> str:
        return rewrite(base, 30 + dumps, 1 if dumps <= 1 else 2, {i: {"try": "2", "out": "none"} for i in (1, 2, 3)})

    app = App(FakeCli(sample, script_stuck), sleep=fake_sleep, clock=lambda: clock[0])
    out = io.StringIO()
    check(app.change_and_wait(SIG_POWER, 0, 5.0, out) == EXIT_TIMEOUT, "deadline gives exit 3")

    # desired state already set: no revision change, every targeted lamp at that revision
    at_rev1 = {2: {"rev": "1", "out": "submitted"}}
    app = App(FakeCli(sample, lambda d, b: rewrite(b, 40 + d, 1, at_rev1)), sleep=fake_sleep, clock=lambda: clock[0])
    out = io.StringIO()
    check(app.change_and_wait(SIG_POWER, 1, 5.0, out) == EXIT_OK, "no change gives exit 0")
    check("no change" in out.getvalue(), "no change reported")

    # another app running: refused before any signal
    fake = FakeCli(sample, script_ok)
    fake.app = "Snake Game"
    app = App(fake, sleep=fake_sleep, clock=lambda: clock[0])
    try:
        app.change_and_wait(SIG_POWER, 1, 5.0, io.StringIO())
        check(False, "foreign app refused")
    except ToolError as exc:
        check(exc.code == EXIT_USAGE and not fake.signals, "foreign app refused without a signal")

    # an unchanged revision still evaluates the targeted lamps (lamp 2 failed at rev 1)
    app = App(FakeCli(sample, lambda d, b: rewrite(b, 50 + d, 1, {})), sleep=fake_sleep, clock=lambda: clock[0])
    out = io.StringIO()
    check(app.change_and_wait(SIG_POWER, 1, 5.0, out) == EXIT_LAMP_FAILED, "unchanged rev with a failed lamp gives exit 1")
    check("no change" in out.getvalue() and "connect_timeout" in out.getvalue(), "no change and the failed lamp reported")

    # a running scan defers the command; the tool keeps polling until the revision moves
    def script_scan(dumps: int, base: str) -> str:
        if dumps <= 3:
            return rewrite(base, 60 + dumps, 1, {}, {"scanning": "0" if dumps <= 1 else "1"})
        return rewrite(base, 60 + dumps, 2, {i: {"rev": "2", "try": "2", "out": "submitted"} for i in (1, 2, 3)})

    fake = FakeCli(sample, script_scan)
    app = App(fake, sleep=fake_sleep, clock=lambda: clock[0])
    out = io.StringIO()
    code = app.change_and_wait(SIG_BRIGHTNESS, 0x30, 30.0, out)
    check(code == EXIT_OK and "no change" not in out.getvalue(), f"scan-deferred command waited for (got {code})")
    check(fake.dumps >= 4, "polled past the scan")

    # connect_timeout while cancelling is not final; the cancel fails afterwards
    def script_cancel(dumps: int, base: str) -> str:
        if dumps <= 1:
            return rewrite(base, 70 + dumps, 1, {})
        ok = {"rev": "2", "try": "2", "out": "submitted"}
        if dumps <= 3:
            lamps = {1: ok, 2: {"try": "2", "out": "connect_timeout"}, 3: ok}
            return rewrite(base, 70 + dumps, 2, lamps, {"engine.state": "cancelling"})
        lamps = {1: ok, 2: {"try": "2", "out": "cancel_failed"}, 3: ok}
        return rewrite(base, 70 + dumps, 2, lamps, {"engine.state": "faulted", "engine.fault": "cancel"})

    fake = FakeCli(sample, script_cancel)
    app = App(fake, sleep=fake_sleep, clock=lambda: clock[0])
    out = io.StringIO()
    code = app.change_and_wait(SIG_BRIGHTNESS, 0x31, 30.0, out)
    check(code == EXIT_LAMP_FAILED and "cancel_failed" in out.getvalue(), "final cancel_failed reported")
    check("engine=faulted" in out.getvalue(), "polled until the cancel resolved")

    # The next checks run against an app build without req= (legacy): the tool falls
    # back to arg 0 and the session and sequence rules. Request id checks follow them.
    # The complete file already on the card (seq 80) is not the answer; the app
    # answers late, so the request is repeated and the answer to the latest one is taken
    fake = FakeCli(sample, lambda d, b: rewrite(b, 80 + d, 1, {}))
    fake.legacy = True
    fake.lag_reads = 8
    app = App(fake, sleep=fake_sleep, clock=lambda: clock[0])
    st = app.dump(clock[0] + 10.0)
    status_requests = sum(1 for c in fake.signals if c.split()[2] == str(SIG_STATUS))
    check(app.legacy_status and len(fake.ignored) == 1, "legacy app ignores the request id once, tool falls back to arg 0")
    check(all(c.split()[3] == "0" for c in fake.signals if c.split()[2] == str(SIG_STATUS)), "legacy requests use arg 0")
    check(st.seq != 80, f"stale dump not accepted (got seq {st.seq})")
    check(status_requests >= 2, f"dump requested again ({status_requests} requests)")
    check(st.seq == 80 + status_requests, "answer to the latest request accepted")
    # counter restarted in a new app session: a smaller number is a new dump
    fake = FakeCli(sample, lambda d, b: rewrite(b, 90 if d == 0 else d, 1, {}, None if d == 0 else {"session": "77"}))
    fake.legacy = True
    app = App(fake, sleep=fake_sleep, clock=lambda: clock[0])
    check(app.dump(clock[0] + 10.0).seq == 1, "restarted counter accepted")
    # the previous session left sequence 1; the new session answers with sequence 1
    for old_session in ("5", None):
        def restart(d: int, b: str, old_session=old_session) -> str:
            if d == 0:
                text = rewrite(b, 1, 1, {}, {"session": old_session} if old_session else None)
                return text if old_session else "\n".join(ln for ln in text.split("\n") if not ln.startswith("session="))
            return rewrite(b, d, 0, {}, {"session": "77"})
        fake = FakeCli(sample, restart)
        fake.legacy = True
        app = App(fake, sleep=fake_sleep, clock=lambda: clock[0])
        try:
            st = app.dump(clock[0] + 10.0)
            ok = st.seq == 1 and st.session == 77
        except ToolError:
            ok = False
        check(ok, f"restart answered with seq 1 accepted (old session {old_session or 'none'})")
        check(sum(1 for c in fake.signals if c.split()[2] == str(SIG_STATUS)) == 1, "one request was enough")
    # the same session with the same number is the old file, not an answer
    fake = FakeCli(sample, lambda d, b: rewrite(b, 1, 1, {}))
    fake.legacy = True
    app = App(fake, sleep=fake_sleep, clock=lambda: clock[0])
    try:
        app.dump(clock[0] + 2.0)
        check(False, "same session and sequence not accepted")
    except ToolError as exc:
        check(exc.code == EXIT_TIMEOUT, "same session and sequence not accepted")

    # After a restart the card shows a new session whose dump answers an earlier
    # request (a raw 100 sent before this call). The session rule alone would take it;
    # with the request id the tool waits for the dump that echoes its own id.
    def restarted(d: int, b: str) -> str:
        if d == 0:
            return rewrite(b, 3, 1, {}, {"session": "5"})
        return rewrite(b, d, 1, {}, {"session": "77"})

    fake = FakeCli(sample, restarted)
    fake.req_override = lambda n, f: 0 if n == 1 else None  # dump 1 answers the earlier raw 100
    app = App(fake, sleep=fake_sleep, clock=lambda: clock[0])
    st = app.dump(clock[0] + 10.0)
    status_args = fake.status_args
    check(not app.legacy_status and status_args[0] != 0, "status request carries a non-zero id")
    check(st.seq == 2 and st.get_int("req", 0) == status_args[-1], f"earlier request's answer after a restart not accepted (got seq {st.seq})")
    check(len(status_args) == 2 and status_args[0] == status_args[1], "the re-request repeats the same id")

    # A status request the app refuses (its inbox was full) does not switch the tool to the
    # rules for an app without req=: the request is repeated with the same id, and the
    # next dumps are still matched by id, so an answer to an earlier request is skipped.
    fake = FakeCli(sample, restarted)
    fake.reject_status = 1
    app = App(fake, sleep=fake_sleep, clock=lambda: clock[0])
    st = app.dump(clock[0] + 10.0)
    check(len(fake.ignored) == 1 and not app.legacy_status, "refused request: tool keeps request ids")
    check(all(a != 0 for a in fake.status_args), "refused request: no fallback to arg 0")
    check(st.get_int("req", 0) == app.req and st.seq == 1, f"refused request: answer matched by id (got seq {st.seq})")
    answered = len(fake.status_args)
    fake.req_override = lambda n, f: f.status_args[0] if n == answered + 1 else None
    st = app.dump(clock[0] + 10.0)
    check(st.get_int("req", 0) == app.req and st.seq == answered + 2, f"after a refused request an earlier answer is skipped (got seq {st.seq})")

    # Refused while the card still holds a file without req= (written by an older build):
    # the tool falls back to arg 0 once; the app's req=0 answer shows that it echoes ids,
    # so that answer is not taken and the request is sent again with the id.
    fake = FakeCli(sample, restarted)
    fake.reject_status = 1
    fake.no_req_dumps = {0}
    app = App(fake, sleep=fake_sleep, clock=lambda: clock[0])
    st = app.dump(clock[0] + 10.0)
    check(fake.status_args[0] == 0 and app.echoes_req and not app.legacy_status, "refused first request: one arg-0 fallback, then ids again")
    check(st.get_int("req", 0) == app.req and app.req != 0, f"refused first request: answer matched by id (got req {st.get_int('req', 0)})")

    # A newer app build left a complete file with req= on the card; an older build without
    # req= runs now. The stale req= line must not make the tool expect ids: it falls back to
    # arg 0 and takes the older app's answers by the session and sequence rules.
    def stale_newer(d: int, b: str) -> str:
        if d == 0:
            lines = rewrite(b, 3, 1, {}, {"session": "5"}).split("\n")
            lines.insert(1, "req=2864434397")
            return "\n".join(lines)
        return rewrite(b, d, 1, {}, {"session": "77"})

    for lag in (0, 3):
        fake = FakeCli(sample, stale_newer)
        fake.legacy = True
        fake.verbatim_dumps = {0}
        fake.lag_reads = lag  # reads after the request that still show the stale file
        app = App(fake, sleep=fake_sleep, clock=lambda: clock[0])
        try:
            st = app.dump(clock[0] + 10.0)
            ok = st.seq == 1 and st.session == 77 and "req" not in st.kv
        except ToolError:
            ok = False
        check(ok, f"stale req= file from a newer build, older app answers without req= (lag {lag})")
        check(app.legacy_status and not app.echoes_req, f"stale req= file: capability not learned (lag {lag})")
        check(all(a == 0 for a in fake.status_args), f"stale req= file: older app gets arg 0 (lag {lag})")
        try:
            ok = app.dump(clock[0] + 10.0).seq == 2
        except ToolError:
            ok = False
        check(ok, f"stale req= file: next dump in the same run by sequence (lag {lag})")

    # No status file on the card before the request (no baseline). A stale file with a
    # req= id this run never sent appears; the running app does not echo ids. Without a
    # baseline the sequence rule cannot date that file, so the capability is not learned
    # and the app's answer is taken by the rules for an app without req=.
    def stale_no_baseline(d: int, b: str) -> str:
        if d == 1:
            return rewrite(b, 3, 1, {}, {"session": "5", "req": "2864434397"})
        return rewrite(b, d - 1, 1, {}, {"session": "77"})

    fake = FakeCli(sample, stale_no_baseline)
    fake.legacy = True
    fake.absent_dumps = {0}
    fake.verbatim_dumps = {1}
    app = App(fake, sleep=fake_sleep, clock=lambda: clock[0])
    try:
        st = app.dump(clock[0] + 10.0)
        ok = st.session == 77 and "req" not in st.kv
    except ToolError:
        ok = False
    check(ok, "no baseline, stale foreign req= file: app answer without req= taken")
    check(app.legacy_status and not app.echoes_req, "no baseline, stale foreign req= file: capability not learned")
    check(all(a == 0 for a in fake.status_args), "no baseline, stale foreign req= file: app gets arg 0")

    # No status file on the card before the request (no baseline). The app first answers an
    # earlier manual status request (req=0), then the tool's own request. A file with a req=
    # line is matched by id even when the id is 0, so the tool takes the second answer.
    fake = FakeCli(sample, lambda d, b: rewrite(b, d, 1, {}, {"session": "77"}))
    fake.absent_dumps = {0}
    fake.req_override = lambda n, f: 0 if n == 1 else None  # dump 1 answers the manual request
    app = App(fake, sleep=fake_sleep, clock=lambda: clock[0])
    try:
        st = app.dump(clock[0] + 10.0)
        ok = st.seq == 2 and st.get_int("req", 0) == app.req and app.req != 0
    except ToolError:
        ok = False
    check(ok, "no baseline, earlier manual req=0 answer first: the tool's own answer taken")

    # No status file on the card before the request (no baseline), and the app refuses the
    # first request (inbox full). The tool falls back to arg 0; the app answers with req=0.
    # In the fallback a req=0 file is judged by session and sequence, so it is taken.
    fake = FakeCli(sample, lambda d, b: rewrite(b, d, 1, {}, {"session": "77"}))
    fake.absent_dumps = {0}
    fake.reject_status = 1
    app = App(fake, sleep=fake_sleep, clock=lambda: clock[0])
    try:
        st = app.dump(clock[0] + 10.0)
        ok = st.seq == 1 and "req" in st.kv and st.get_int("req", -1) == 0
    except ToolError:
        ok = False
    check(ok, "no baseline, first request refused: req=0 answer in the fallback taken, no timeout")
    check(app.legacy_status and fake.status_args == [0], "no baseline, first request refused: one arg-0 request")

    # Request ids end to end: the dump after the command first shows the answer to the request of
    # the dump before it (pre-command state, rev 0, new session); it must not decide the result
    def script_after_restart(dumps: int, base: str) -> str:
        if dumps == 0:
            return rewrite(base, 3, 1, {}, {"session": "5"})
        if dumps <= 2:
            return rewrite(base, dumps, 0, {i: {"rev": "0", "try": "0", "out": "none"} for i in (1, 2, 3)}, {"session": "77"})
        return rewrite(base, dumps, 1, {i: {"rev": "1", "try": "1", "out": "submitted"} for i in (1, 2, 3)}, {"session": "77"})

    fake = FakeCli(sample, script_after_restart)
    fake.req_override = lambda n, f: f.status_args[0] if n == 2 else None
    app = App(fake, sleep=fake_sleep, clock=lambda: clock[0])
    out = io.StringIO()
    code = app.change_and_wait(SIG_POWER, 1, 30.0, out)
    check(code == EXIT_OK and "rev 0" not in out.getvalue(), f"pre-command answer after a restart ignored (got {code})")
    check(len(set(fake.status_args)) >= 2, "each dump uses its own request id")

    # revision 0 with nothing submitted is not a success
    app = App(FakeCli(sample, lambda d, b: rewrite(b, 130 + d, 0, {i: {"rev": "0", "try": "0", "out": "none"} for i in (1, 2, 3)})), sleep=fake_sleep, clock=lambda: clock[0])
    out = io.StringIO()
    code = app.change_and_wait(SIG_POWER, 1, 5.0, out)
    check(code == EXIT_LAMP_FAILED and "rev 0" in out.getvalue(), f"rev 0 gives exit 1 (got {code})")
    # the app answers a command at rev 0 with the full state (rev 1); all lamps reach it
    def script_first(dumps: int, base: str) -> str:
        if dumps <= 1:
            return rewrite(base, 140 + dumps, 0, {i: {"rev": "0", "try": "0", "out": "none"} for i in (1, 2, 3)})
        return rewrite(base, 140 + dumps, 1, {i: {"rev": "1", "try": "1", "out": "submitted"} for i in (1, 2, 3)})
    app = App(FakeCli(sample, script_first), sleep=fake_sleep, clock=lambda: clock[0])
    check(app.change_and_wait(SIG_POWER, 1, 5.0, io.StringIO()) == EXIT_OK, "first command at rev 0 sent in full gives 0")

    # the app check runs before every signal; another app opened mid-command
    def switch(fake_cli, signal: int) -> None:
        if fake_cli.dumps >= 3 and fake_cli.app != "Snake Game":
            fake_cli.app = "Snake Game"
            fake_cli.switched_at = len(fake_cli.signals)

    fake = FakeCli(sample, script_stuck)
    fake.after_signal = switch
    app = App(fake, sleep=fake_sleep, clock=lambda: clock[0])
    sent = 0
    try:
        app.change_and_wait(SIG_POWER, 0, 30.0, io.StringIO())
        check(False, "app switch detected")
    except ToolError as exc:
        sent = len(fake.signals)
        check(exc.code == EXIT_USAGE and "Snake Game" in str(exc), "app switch detected")
    check(sent == getattr(fake, "switched_at", -1), f"no signal sent to the other app ({sent} signals)")

    # rejected target, lightshow and forget give a non-zero exit code
    def ns(command: str, *rest: str):
        return argparse.Namespace(command=command, args=list(rest), timeout=5.0, clear=False, fap=DEFAULT_FAP)

    plain = lambda d, b: rewrite(b, 100 + d, 1, {})  # noqa: E731
    for argv, want in (
        (("target", "5"), EXIT_USAGE),
        (("target", "0"), EXIT_OK),
        (("lightshow", "on"), EXIT_USAGE),
        (("lightshow", "off"), EXIT_OK),
        (("forget", "9"), EXIT_USAGE),
        (("forget", "2"), EXIT_USAGE),
    ):
        out = io.StringIO()
        code = run_command(App(FakeCli(sample, plain), sleep=fake_sleep, clock=lambda: clock[0]), ns(*argv), out)
        check(code == want, f"{' '.join(argv)} gives {want} (got {code})")
        if want != EXIT_OK:
            check("rejected by the app" in out.getvalue(), f"{' '.join(argv)} explains the rejection")

    def script_forget(dumps: int, base: str) -> str:
        if dumps <= 1:
            return rewrite(base, 110 + dumps, 1, {})
        lines = [ln for ln in rewrite(base, 110 + dumps, 1, {}).split("\n") if not ln.startswith("lamp.3=")]
        text = "\n".join(lines).replace("registry.count=3", "registry.count=2").replace("lamps=3", "lamps=2")
        return text.replace("addr=A4:C1:38:00:00:02", "addr=A4:C1:38:00:00:03")

    out = io.StringIO()
    code = run_command(App(FakeCli(sample, script_forget), sleep=fake_sleep, clock=lambda: clock[0]), ns("forget", "2"), out)
    check(code == EXIT_OK, f"accepted forget gives 0 (got {code})")

    # scan: a refused scan start gives a non-zero exit code; a file without scan.refused
    # (app build without the line) counts as not refused
    for refused, want in (("1", EXIT_USAGE), ("0", EXIT_OK), (None, EXIT_OK)):
        def script_scan_end(d: int, b: str, refused=refused) -> str:
            text = rewrite(b, 150 + d, 1, {}, {"scanning": "0", "scan.refused": refused or "0"})
            return text if refused else "\n".join(ln for ln in text.split("\n") if not ln.startswith("scan.refused="))
        out = io.StringIO()
        code = run_command(App(FakeCli(sample, script_scan_end), sleep=fake_sleep, clock=lambda: clock[0]), ns("scan"), out)
        check(code == want, f"scan with scan.refused={refused} gives {want} (got {code})")
        check(("scan refused" in out.getvalue()) == (refused == "1"), f"scan with scan.refused={refused} explains a refusal")
    check(parse_status(sample).kv.get("scan.refused") == "0", "sample has scan.refused=0")

    # close only succeeds when the app is gone
    fake = FakeCli(sample, plain)
    check(run_command(App(fake, sleep=fake_sleep, clock=lambda: clock[0]), ns("close"), io.StringIO()) == EXIT_OK, "closed app gives 0")
    # "was closed" while the app still tears down: wait until it is gone
    fake = FakeCli(sample, plain)
    fake.close_linger = 10
    code = run_command(App(fake, sleep=fake_sleep, clock=lambda: clock[0]), ns("close"), io.StringIO())
    check(code == EXIT_OK and fake.app is None, f"close waits for the app to end (got {code})")
    fake = FakeCli(sample, plain)
    fake.close_linger = 10**6
    t0 = clock[0]
    try:
        run_command(App(fake, sleep=fake_sleep, clock=lambda: clock[0]), ns("close"), io.StringIO())
        check(False, "app that keeps running after was closed is an error")
    except ToolError as exc:
        check(exc.code == EXIT_USAGE and clock[0] - t0 >= CLOSE_WAIT_S, "app that keeps running after was closed is an error")
    fake = FakeCli(sample, plain)
    fake.close_reply = "manual"
    try:
        run_command(App(fake, sleep=fake_sleep, clock=lambda: clock[0]), ns("close"), io.StringIO())
        check(False, "refused close is an error")
    except ToolError as exc:
        check(exc.code == EXIT_USAGE, "refused close is an error")

    # serial failure ends with the I/O exit code and one line, no traceback
    import contextlib

    fake = FakeCli(sample, plain)
    fake.fail_io = True
    err = io.StringIO()
    with contextlib.redirect_stderr(err):
        code = run_guarded(lambda: run_command(App(fake, sleep=fake_sleep, clock=lambda: clock[0]), ns("status"), io.StringIO()))
    check(code == EXIT_IO, f"serial failure gives exit 4 (got {code})")
    check(err.getvalue().count("\n") == 1 and "Traceback" not in err.getvalue(), "one-line message")

    print(f"selftest: {checks} checks, {failures} failed")
    return EXIT_OK if failures == 0 else EXIT_LAMP_FAILED


# --------------------------------------------------------------------------


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", help="serial port (default: the one /dev/cu.usbmodemflip_*)")
    parser.add_argument("--timeout", type=float, default=30.0, help="deadline in seconds (default 30)")
    parser.add_argument("--fap", default=DEFAULT_FAP, help=f"app path for open (default {DEFAULT_FAP})")
    parser.add_argument(
        "--fixtures",
        default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "tests", "fixtures"),
        help="fixture directory for selftest",
    )
    parser.add_argument("--clear", action="store_true", help="events: clear the ring after printing")
    parser.add_argument("command")
    parser.add_argument("args", nargs="*")
    args = parser.parse_args(argv)

    if args.command == "selftest":
        return selftest(args.fixtures)

    def device() -> int:
        cli = FlipperCli(args.port or find_port())
        try:
            return run_command(App(cli), args, sys.stdout)
        finally:
            cli.close()

    return run_guarded(device)


def run_guarded(fn) -> int:
    """Run fn; tool and serial errors end with their exit code and one line."""
    try:
        return fn()
    except ToolError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return exc.code
    except OSError as exc:  # pyserial's SerialException is an OSError
        print(f"error: serial I/O failed: {exc}", file=sys.stderr)
        return EXIT_IO


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
