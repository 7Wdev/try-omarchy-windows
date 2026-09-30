"""Terminal prompts: gum when it is available (it ships with Omarchy), plain
input() otherwise, and fixed answers when running unattended."""

import os
import shutil
import subprocess
import sys


class Cancelled(Exception):
    pass


class UI:
    def __init__(self, interactive=True, use_gum=None, stream=None):
        self.stream = stream or sys.stdout
        self.interactive = interactive and sys.stdin.isatty() and self.stream.isatty()
        if use_gum is None:
            use_gum = self.interactive and shutil.which("gum") is not None
        self.gum = use_gum

    # Output ---------------------------------------------------------------

    def say(self, text=""):
        print(text, file=self.stream, flush=True)

    def heading(self, text):
        if self.gum:
            subprocess.run(["gum", "style", "--bold", "--foreground", "212", text])
        else:
            self.say(text)
            self.say("=" * len(text))

    def warn(self, text):
        if self.gum:
            subprocess.run(["gum", "style", "--foreground", "214", text])
        else:
            self.say(f"Warning: {text}")

    def error(self, text):
        print(f"Error: {text}", file=sys.stderr, flush=True)

    def progress(self, done, total, label=""):
        if not self.stream.isatty():
            if done == total or done % 500 == 0:
                self.say(f"  {done}/{total}")
            return
        width = shutil.get_terminal_size((80, 20)).columns
        text = f"  {done}/{total} {label}"
        if len(text) > width - 1:
            text = text[:width - 2] + "…"
        print(f"\r\033[K{text}", end="" if done < total else "\n", file=self.stream, flush=True)

    def pager(self, text):
        if self.interactive and shutil.which("less"):
            subprocess.run(["less", "-R", "-F", "-X"], input=text, text=True)
        else:
            self.say(text)

    # Questions ------------------------------------------------------------

    def confirm(self, question, default=True):
        if not self.interactive:
            return default
        if self.gum:
            argv = ["gum", "confirm", question]
            if not default:
                argv.append("--default=false")
            result = subprocess.run(argv)
            if result.returncode == 130:
                raise Cancelled()
            return result.returncode == 0
        answer = self._input(f"{question} [{'Y/n' if default else 'y/N'}] ").strip().lower()
        if not answer:
            return default
        return answer in ("y", "yes")

    def choose_one(self, question, options, default=0):
        if not self.interactive or len(options) == 1:
            return default
        if self.gum:
            result = subprocess.run(["gum", "choose", "--header", question,
                                     "--selected", options[default], "--", *options],
                                    stdout=subprocess.PIPE, text=True)
            if result.returncode != 0:
                raise Cancelled()
            chosen = result.stdout.rstrip("\n")
            return options.index(chosen) if chosen in options else default
        self.say(question)
        for index, option in enumerate(options, start=1):
            self.say(f"  {index}. {option}")
        while True:
            answer = self._input(f"Choose 1-{len(options)} [{default + 1}]: ").strip()
            if not answer:
                return default
            if answer.isdigit() and 1 <= int(answer) <= len(options):
                return int(answer) - 1

    def choose_many(self, question, options, selected):
        """Return a list of booleans, one per option."""
        if not self.interactive:
            return list(selected)
        if self.gum:
            argv = ["gum", "choose", "--no-limit", "--header", question,
                    "--height", str(min(len(options) + 2, 24))]
            picked = [option for option, on in zip(options, selected) if on]
            if picked:
                argv += ["--selected", ",".join(picked)]
            result = subprocess.run([*argv, "--", *options], stdout=subprocess.PIPE, text=True)
            if result.returncode != 0:
                raise Cancelled()
            chosen = set(result.stdout.splitlines())
            return [option in chosen for option in options]
        state = list(selected)
        while True:
            self.say(question)
            for index, (option, on) in enumerate(zip(options, state), start=1):
                self.say(f"  [{'x' if on else ' '}] {index}. {option}")
            answer = self._input("Numbers to switch on or off, then Enter to continue: ").strip()
            if not answer:
                return state
            for token in answer.replace(",", " ").split():
                if token.isdigit() and 1 <= int(token) <= len(state):
                    state[int(token) - 1] = not state[int(token) - 1]

    def _input(self, prompt):
        try:
            return input(prompt)
        except EOFError:
            raise Cancelled() from None
        except KeyboardInterrupt:
            self.say()
            raise Cancelled() from None


def plain_label(text):
    """gum's --selected splits on commas, so labels must not contain any."""
    return text.replace(",", "")


def human_size(size):
    value = float(size)
    for unit in ("bytes", "KB", "MB", "GB", "TB"):
        if value < 1000 or unit == "TB":
            if unit == "bytes":
                return f"{int(value)} bytes"
            return f"{value:.1f} {unit}" if value < 10 else f"{value:.0f} {unit}"
        value /= 1000
    return f"{size} bytes"


def terminal_is_interactive():
    return sys.stdin.isatty() and sys.stdout.isatty() and os.environ.get("TERM") != "dumb"
