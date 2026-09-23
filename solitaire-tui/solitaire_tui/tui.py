"""The terminal game: keyboard and mouse Klondike in curses.

Drawing is split in two: `layout(game, w, h)` is a pure function returning
the cells to draw and the clickable regions, so both are testable without a
terminal; `run()` owns curses, colours and input. Every move goes through
engine.apply, so the UI can never do anything the rules do not allow.

Mouse input is read as SGR (1006) reports the game enables itself, rather
than through curses' terminfo-dependent mouse layer, so clicks, drags and the
on-screen buttons behave the same in every terminal that speaks SGR.
"""
from __future__ import annotations

import curses
import locale
import os
import random
import sys
import time
from dataclasses import dataclass, field
from typing import Optional

from . import engine
from .agents import GreedyAgent
from .engine import (FOUND_TAB, TAB_FOUND, TAB_TAB, TABLEAU, WASTE_FOUND,
                     WASTE_TAB, Move, State)

CW, CH = 7, 5            # card width and height, borders included
CELL = CW + 1            # columns per pile
ORIGIN_X = 2
TOP_Y = 2                # stock / waste / foundations
TAB_Y = TOP_Y + CH + 1   # first tableau row
FAN_STEP = 2             # deal-three waste fan offset
DOUBLE_CLICK = 0.4       # seconds
MIN_W = ORIGIN_X + TABLEAU * CELL + 1

TOP, TAB = "top", "tab"
STOCK, WASTE = 0, 1      # top-row positions; 2..5 are foundations (suits 0..3)
TOP_COLUMN = (0, 1, 3, 4, 5, 6)   # screen column of each top-row position

BUTTONS = (("deal", "Deal"), ("auto", "Auto"), ("home", "All home"),
           ("undo", "Undo"), ("hint", "Hint"), ("new", "New"),
           ("help", "Help"), ("quit", "Quit"))

HELP = [
    "Mouse: click a card to pick it up, click where it goes (or drag it).",
    "       Double- or right-click a card to auto-place it.",
    "arrows      move; Up/Down on a column widens/narrows the run",
    "space/enter pick up, drop, or deal from the stock      esc  let go",
    "a  auto-place the selected card    A  send everything home",
    "1-7  jump to (or drop on) a column    f  send the card home",
    "d deal   u undo   h hint   r restart   n new deal   3 deal one/three   q quit",
]


def _label(c: int) -> str:
    return engine.RANK_NAMES[engine.rank_of(c)] + engine.SUIT_SYMBOLS[engine.suit_of(c)]


def card_rows(c: Optional[int], up: bool = True, slot: str = " ") -> list:
    """The five rows of a card, a card back, or an empty slot."""
    if c is None:
        return ["╭─────╮", "│     │",
                f"│  {slot}  │", "│     │",
                "╰─────╯"]
    if not up:
        return ["╭─────╮"] + \
               ["│░░░░░│"] * 3 + \
               ["╰─────╯"]
    lab, suit = _label(c), engine.SUIT_SYMBOLS[engine.suit_of(c)]
    return ["╭─────╮", f"│{lab:<5}│",
            f"│  {suit}  │", f"│{lab:>5}│",
            "╰─────╯"]


def tab_border(c: int) -> str:
    """A covered face-up card in compact layout: its label set in the border."""
    return f"╭{_label(c):─<5}╮"


def card_text(c: Optional[int]) -> str:
    """Short label, e.g. for messages and tests."""
    return "--" if c is None else _label(c)


@dataclass
class Game:
    state: State
    area: str = TAB
    pos: int = 0             # top-row position or tableau column
    depth: int = 1           # cards selected from the top of a tableau column
    held: Optional[tuple] = None   # ("waste",) | ("found", s) | ("tab", i, n)
    message: str = ""
    undo: list = field(default_factory=list)
    hint: Optional[Move] = None
    started: float = field(default_factory=time.monotonic)
    show_help: bool = False
    quit: bool = False
    hits: list = field(default_factory=list)      # from the last layout()
    press: Optional[dict] = None                   # mouse press in progress
    last_click: tuple = (None, 0.0)                # (target, time) for double-click

    @classmethod
    def new(cls, seed: Optional[int] = None, draw3: bool = False) -> "Game":
        if seed is None:
            seed = random.SystemRandom().randrange(1, 10**9)
        return cls(engine.deal(seed, draw3))

    def _reset_to(self, fresh: "Game") -> None:
        self.__dict__.update(fresh.__dict__)

    def _clamp_depth(self) -> None:
        if self.area == TAB:
            self.depth = max(1, min(self.depth, len(self.state.face_up(self.pos)) or 1))

    def play(self, move: Move, quiet: bool = False) -> bool:
        if not engine.is_legal(self.state, move):
            if not quiet:
                self.message = f"can't: {move}"
            return False
        self.undo.append(self.state)
        self.state = engine.apply(self.state, move)
        self.held = None
        self.hint = None
        self.message = "You won! n = new deal" if self.state.won else ""
        self._clamp_depth()
        return True

    # ------------------------------------------------------------ moves
    def deal(self) -> None:
        if not (self.state.stock or self.state.waste):
            self.message = "the stock is empty"
            return
        self.play(Move(engine.DEAL))

    def drop_on_tab(self, j: int) -> None:
        h = self.held
        if h[0] == "waste":
            move = Move(WASTE_TAB, dst=j)
        elif h[0] == "found":
            move = Move(FOUND_TAB, src=h[1], dst=j)
        else:
            if h[1] == j:
                self.held = None
                return
            move = Move(TAB_TAB, src=h[1], dst=j, count=h[2])
        self.play(move)

    def drop_on_foundation(self) -> None:
        h = self.held
        if h[0] == "waste":
            self.play(Move(WASTE_FOUND))
        elif h[0] == "tab" and h[2] == 1:
            self.play(Move(TAB_FOUND, src=h[1]))
        elif h[0] == "found":
            self.held = None
        else:
            self.message = "only a single card goes home"

    def selection(self) -> Optional[tuple]:
        """What auto-place acts on: the held cards, else the cursor's."""
        if self.held is not None:
            return self.held
        s = self.state
        if self.area == TOP:
            if self.pos == WASTE and s.waste:
                return ("waste",)
            if self.pos >= 2 and s.found[self.pos - 2]:
                return ("found", self.pos - 2)
            return None
        up = s.face_up(self.pos)
        return ("tab", self.pos, min(self.depth, len(up))) if up else None

    def auto_move(self, sel: Optional[tuple] = None) -> Optional[Move]:
        """The likely place for a selection: home first, then the best column.

        Among tableau destinations it prefers a move that uncovers a face-down
        card, then building on a card over filling a gap, then the nearest
        column. It never moves a King from one gap to another.
        """
        sel = sel or self.selection()
        if sel is None:
            return None
        s = self.state
        legal = engine.legal_moves(s)
        if sel[0] == "waste":
            home = Move(WASTE_FOUND)
            onto = [m for m in legal if m.kind == WASTE_TAB]
            src_col = 1
        elif sel[0] == "found":
            home = None
            onto = [m for m in legal if m.kind == FOUND_TAB and m.src == sel[1]]
            src_col = TOP_COLUMN[2 + sel[1]]
        else:
            _, i, n = sel
            home = Move(TAB_FOUND, src=i) if n == 1 else None
            onto = [m for m in legal if m.kind == TAB_TAB and m.src == i and m.count == n]
            src_col = i
        if home is not None and home in legal:
            return home

        def score(m: Move):
            reveals = (m.kind == TAB_TAB and m.count == len(s.face_up(m.src))
                       and s.down[m.src] > 0)
            builds = bool(s.tableau[m.dst])
            pointless = (m.kind == TAB_TAB and not builds and s.down[m.src] == 0
                         and m.count == len(s.tableau[m.src]))
            return (not pointless, reveals, builds, -abs(m.dst - src_col))

        onto = [m for m in onto if score(m)[0]]
        return max(onto, key=score) if onto else None

    def auto_place(self, sel: Optional[tuple] = None) -> None:
        sel = sel or self.selection()
        move = self.auto_move(sel)
        if move is None:
            self.message = "nothing to place" if sel is None else "no place for that"
            self.held = None
            return
        label = self._selection_label(sel)
        if self.play(move):
            where = "home" if move.kind in (WASTE_FOUND, TAB_FOUND) else f"column {move.dst + 1}"
            self.message = f"{label} → {where}"

    def _selection_label(self, sel: tuple) -> str:
        s = self.state
        if sel[0] == "waste":
            return card_text(s.waste_top())
        if sel[0] == "found":
            return card_text(s.found_top(sel[1]))
        _, i, n = sel
        return card_text(s.tableau[i][-n])

    def activate(self) -> None:
        """Space/Enter at the cursor: deal, pick up, or drop."""
        s = self.state
        if self.held is not None:
            if self.area == TAB:
                self.drop_on_tab(self.pos)
            elif self.pos >= 2:
                self.drop_on_foundation()
            else:
                self.held = None
            return
        if self.area == TOP:
            if self.pos == STOCK:
                self.deal()
            elif self.pos == WASTE:
                if s.waste:
                    self.held = ("waste",)
            elif s.found[self.pos - 2]:
                self.held = ("found", self.pos - 2)
            return
        up = s.face_up(self.pos)
        if up:
            self.held = ("tab", self.pos, min(self.depth, len(up)))

    def send_home(self) -> None:
        s = self.state
        if self.area == TOP and self.pos == WASTE and s.waste:
            self.play(Move(WASTE_FOUND))
        elif self.area == TAB and s.tableau[self.pos]:
            self.play(Move(TAB_FOUND, src=self.pos))
        else:
            self.message = "nothing to send home here"

    def all_home(self) -> None:
        moved = 0
        while True:
            homeward = [m for m in engine.legal_moves(self.state)
                        if m.kind in (WASTE_FOUND, TAB_FOUND)]
            if not homeward or not self.play(homeward[0]):
                break
            moved += 1
        if not self.state.won:
            self.message = f"sent {moved} card{'s' if moved != 1 else ''} home"

    def do_undo(self) -> None:
        if self.undo:
            self.state = self.undo.pop()
            self.held = None
            self.message = "undone"
            self._clamp_depth()
        else:
            self.message = "nothing to undo"

    def show_hint(self) -> None:
        move = GreedyAgent().choose(self.state, engine.legal_moves(self.state))
        self.hint = move
        self.message = f"hint: {move}" if move else "no hint: try undo or a new deal"

    def button(self, name: str) -> None:
        {"deal": self.deal, "auto": self.auto_place, "home": self.all_home,
         "undo": self.do_undo, "hint": self.show_hint,
         "new": lambda: self._reset_to(Game.new(draw3=self.state.draw3)),
         "help": lambda: setattr(self, "show_help", True),
         "quit": lambda: setattr(self, "quit", True)}[name]()

    # ------------------------------------------------------------ cursor
    def move_horizontal(self, step: int) -> None:
        count = 6 if self.area == TOP else TABLEAU
        self.pos = (self.pos + step) % count
        self.depth = 1

    def move_up(self) -> None:
        if self.area == TAB:
            up = self.state.face_up(self.pos)
            if self.held is None and self.depth < len(up):
                self.depth += 1
                return
            column = self.pos
            self.area = TOP
            self.pos = {0: STOCK, 1: WASTE, 2: WASTE}.get(column, column - 1)
        self.depth = 1

    def move_down(self) -> None:
        if self.area == TOP:
            self.area = TAB
            self.pos = TOP_COLUMN[self.pos]
            self.depth = 1
        elif self.depth > 1:
            self.depth -= 1

    def jump(self, column: int) -> None:
        if self.held is not None:
            self.drop_on_tab(column)
        self.area, self.pos, self.depth = TAB, column, 1

    # ------------------------------------------------------------ mouse
    def hit_at(self, row: int, col: int):
        for y0, y1, x0, x1, target in reversed(self.hits):
            if y0 <= row < y1 and x0 <= col < x1:
                return target
        return None

    def _point_cursor(self, target) -> None:
        kind = target[0]
        if kind == "stock":
            self.area, self.pos, self.depth = TOP, STOCK, 1
        elif kind == "waste":
            self.area, self.pos, self.depth = TOP, WASTE, 1
        elif kind == "found":
            self.area, self.pos, self.depth = TOP, 2 + target[1], 1
        elif kind in ("tab", "tabempty"):
            i = target[1]
            self.area, self.pos = TAB, i
            if kind == "tab":
                self.depth = max(1, len(self.state.tableau[i]) - target[2])

    def _pick_from(self, target) -> Optional[tuple]:
        s, kind = self.state, target[0]
        if kind == "waste" and s.waste:
            return ("waste",)
        if kind == "found" and s.found[target[1]]:
            return ("found", target[1])
        if kind == "tab":
            _, i, j = target
            if j >= s.down[i]:
                return ("tab", i, len(s.tableau[i]) - j)
        return None

    def _drop_on(self, target) -> None:
        kind = target[0]
        if kind in ("tab", "tabempty"):
            self.drop_on_tab(target[1])
        elif kind == "found":
            self.drop_on_foundation()
        else:
            self.held = None

    @staticmethod
    def _pile(target):
        if target is None:
            return None
        return target[:2] if target[0] in ("tab", "tabempty", "found") else target[:1]

    def mouse_down(self, row: int, col: int, button: int = 0) -> None:
        target = self.hit_at(row, col)
        self.press = None
        if self.show_help:
            self.show_help = False
            return
        if target is None:
            self.held = None
            return
        if target[0] == "button":
            self.button(target[1])
            return
        if button == 2:                              # right click: auto-place
            sel = self._pick_from(target)
            self._point_cursor(target)
            self.auto_place(sel) if sel else None
            return
        now = time.monotonic()
        prev, when = self.last_click
        self.last_click = (target, now)
        if prev == target and now - when < DOUBLE_CLICK:
            sel = self.held or self._pick_from(target)
            if sel:
                self.auto_place(sel)
            return
        self._point_cursor(target)
        if target[0] == "stock":
            self.held = None
            self.deal()
            return
        if self.held is None:
            picked = self._pick_from(target)
            self.held = picked
            self.press = {"picked": picked is not None, "pile": self._pile(target)}
        else:
            self.press = {"picked": False, "pile": self._pile(target)}

    def mouse_up(self, row: int, col: int) -> None:
        press, self.press = self.press, None
        if press is None or self.held is None:
            return
        target = self.hit_at(row, col)
        pile = self._pile(target)
        if press["picked"] and pile == press["pile"]:
            return                                   # a click: keep it selected
        if target is None or target[0] in ("button", "stock"):
            self.held = None
            return
        held_pile = {"waste": ("waste",), "found": ("found", self.held[-1]),
                     "tab": ("tab", self.held[1])}[self.held[0]]
        if pile in (held_pile, ("tabempty",) + held_pile[1:]):
            self.held = None                         # dropped back where it was
            return
        self._drop_on(target)

    # ------------------------------------------------------------ keys
    def key(self, ch: int) -> None:
        if self.show_help:
            self.show_help = False
            return
        if not self.state.won:
            self.message = ""
        if ch == curses.KEY_LEFT:
            self.move_horizontal(-1)
        elif ch == curses.KEY_RIGHT:
            self.move_horizontal(1)
        elif ch == curses.KEY_UP:
            self.move_up()
        elif ch == curses.KEY_DOWN:
            self.move_down()
        elif ch in (ord(" "), ord("\n"), curses.KEY_ENTER, 13):
            self.activate()
        elif ch == 27:
            self.held = None
        elif ord("1") <= ch <= ord("7"):
            self.jump(ch - ord("1"))
        elif ch == ord("a"):
            self.auto_place()
        elif ch == ord("A"):
            self.all_home()
        elif ch == ord("f"):
            self.send_home()
        elif ch == ord("d"):
            self.deal()
        elif ch == ord("u"):
            self.do_undo()
        elif ch == ord("h"):
            self.show_hint()
        elif ch == ord("n"):
            self._reset_to(Game.new(draw3=self.state.draw3))
        elif ch == ord("r"):
            self._reset_to(Game(engine.deal(self.state.seed, self.state.draw3)))
        elif ch == ord("3"):
            self._reset_to(Game.new(draw3=not self.state.draw3))
            self.message = "deal three" if self.state.draw3 else "deal one"
        elif ch == ord("?"):
            self.show_help = True
        elif ch in (ord("q"), ord("Q")):
            self.quit = True


# ---------------------------------------------------------------- layout

def _column_extent(s: State, i: int, up_step: int) -> int:
    pile = s.tableau[i]
    if not pile:
        return CH
    ups = len(pile) - s.down[i]
    return s.down[i] + (ups - 1) * up_step + CH


def layout(game: Game, width: int = 80, height: int = 40):
    """(cells, hits): cells are (row, col, text, style); hits are
    (y0, y1, x0, x1, target), later entries on top. Pure; no curses."""
    s = game.state
    cells, hits = [], []
    x = lambda column: ORIGIN_X + column * CELL          # noqa: E731

    deepest = max(_column_extent(s, i, 2) for i in range(TABLEAU))
    footer = 4
    up_step = 2 if TAB_Y + deepest + footer <= height else 1

    held = set()
    if game.held:
        h = game.held
        if h[0] == "waste":
            held.add(("waste",))
        elif h[0] == "found":
            held.add(("found", h[1]))
        else:
            for j in range(len(s.tableau[h[1]]) - h[2], len(s.tableau[h[1]])):
                held.add(("tab", h[1], j))

    def under_cursor(where) -> bool:
        kind = where[0]
        if kind == "stock":
            return game.area == TOP and game.pos == STOCK
        if kind == "waste":
            return game.area == TOP and game.pos == WASTE
        if kind == "found":
            return game.area == TOP and game.pos == 2 + where[1]
        if kind == "tabempty":
            return game.area == TAB and game.pos == where[1]
        _, i, j = where
        return (game.area == TAB and game.pos == i
                and j >= len(s.tableau[i]) - game.depth)

    def style(c, where, base):
        if where in held:
            return "held"
        if under_cursor(where):
            return "cursor"
        return base

    def face_style(c):
        return "red" if engine.is_red(c) else "black"

    def put_card(y, x0, rows, st, where, visible=CH):
        for k, text in enumerate(rows[:visible]):
            cells.append((y + k, x0, text, st))
        hits.append((y, y + visible, x0, x0 + CW, where))

    elapsed = int(time.monotonic() - game.started)
    mode = "deal three" if s.draw3 else "deal one"
    cells.append((0, ORIGIN_X, f"SOLITAIRE  seed {s.seed}  {mode}  moves {s.moves}  "
                  f"time {elapsed // 60}:{elapsed % 60:02d}  home {s.cards_home()}/52",
                  "title"))

    # stock
    where = ("stock",)
    if s.stock:
        put_card(TOP_Y, x(0), card_rows(0, up=False),
                 "cursor" if under_cursor(where) else "back", where)
    else:
        put_card(TOP_Y, x(0), card_rows(None, slot="↻"),
                 "cursor" if under_cursor(where) else "slot", where)
    # waste
    where = ("waste",)
    if s.waste:
        n = max(1, min(s.fan, len(s.waste))) if s.draw3 else 1
        shown = s.waste[-n:]
        for k, c in enumerate(shown):
            top = k == len(shown) - 1
            st = style(c, where, face_style(c)) if top else face_style(c)
            put_card(TOP_Y, x(1) + k * FAN_STEP, card_rows(c), st, where)
    else:
        put_card(TOP_Y, x(1), card_rows(None), "cursor" if under_cursor(where) else "slot",
                 where)
    # foundations
    for suit in range(4):
        where = ("found", suit)
        top = s.found_top(suit)
        if top is None:
            put_card(TOP_Y, x(TOP_COLUMN[2 + suit]),
                     card_rows(None, slot=engine.SUIT_SYMBOLS[suit]),
                     "cursor" if under_cursor(where) else "slot", where)
        else:
            put_card(TOP_Y, x(TOP_COLUMN[2 + suit]), card_rows(top),
                     style(top, where, face_style(top)), where)
    # tableau
    for i in range(TABLEAU):
        pile = s.tableau[i]
        if not pile:
            where = ("tabempty", i)
            put_card(TAB_Y, x(i), card_rows(None),
                     "cursor" if under_cursor(where) else "slot", where)
            continue
        y = TAB_Y
        for j, c in enumerate(pile):
            where = ("tab", i, j)
            last = j == len(pile) - 1
            if j < s.down[i]:
                rows, st, step = card_rows(c, up=False), "back", 1
            else:
                rows, st, step = card_rows(c), style(c, where, face_style(c)), up_step
                if up_step == 1 and not last:
                    rows = [tab_border(c)] + rows[1:]
            put_card(y, x(i), rows, st, where, CH if last else step)
            y += step

    # hint, message, buttons, key help
    if game.hint is not None:
        cells.append((height - 4, ORIGIN_X, f"hint: {game.hint}", "hint"))
    cells.append((height - 3, ORIGIN_X, game.message, "msg"))
    bx = ORIGIN_X
    for name, label in BUTTONS:
        text = f"[ {label} ]"
        cells.append((height - 2, bx, text, "button"))
        hits.append((height - 2, height - 1, bx, bx + len(text), ("button", name)))
        bx += len(text) + 1
    cells.append((height - 1, ORIGIN_X,
                  "click/drag cards · double- or right-click or a: auto-place · "
                  "arrows+space · ? help", "dim"))
    if game.show_help:
        top = max(TAB_Y, (height - len(HELP) - 3) // 2)
        lines = ["SOLITAIRE — KEYS AND MOUSE", ""] + HELP + ["", "(any key closes)"]
        for k, line in enumerate(lines):
            cells.append((top + k, ORIGIN_X + 1, f" {line:<72} ", "help"))
    return cells, hits


def render(game: Game, width: int = 80, height: int = 40) -> list:
    return layout(game, width, height)[0]


def min_height(game: Game) -> int:
    s = game.state
    return TAB_Y + max(_column_extent(s, i, 1) for i in range(TABLEAU)) + 4


# ---------------------------------------------------------------- curses

MOUSE_ON = "\x1b[?1000h\x1b[?1002h\x1b[?1006h"
MOUSE_OFF = "\x1b[?1006l\x1b[?1002l\x1b[?1000l"


def _init_colors() -> dict:
    curses.start_color()
    try:
        curses.use_default_colors()
        bg = -1
    except curses.error:
        bg = curses.COLOR_BLACK
    white = curses.COLOR_WHITE
    pairs = {
        "plain": (-1 if bg == -1 else white, bg, 0),
        "title": (curses.COLOR_YELLOW, bg, curses.A_BOLD),
        "red": (curses.COLOR_RED, white, curses.A_BOLD),
        "black": (curses.COLOR_BLACK, white, curses.A_BOLD),
        "back": (white, curses.COLOR_BLUE, 0),
        "slot": (-1 if bg == -1 else white, bg, curses.A_DIM),
        "cursor": (curses.COLOR_BLACK, curses.COLOR_YELLOW, curses.A_BOLD),
        "held": (curses.COLOR_BLACK, curses.COLOR_CYAN, curses.A_BOLD),
        "dim": (-1 if bg == -1 else white, bg, curses.A_DIM),
        "msg": (curses.COLOR_YELLOW, bg, curses.A_BOLD),
        "hint": (curses.COLOR_CYAN, bg, curses.A_BOLD),
        "help": (curses.COLOR_BLACK, white, 0),
        "button": (curses.COLOR_BLACK, curses.COLOR_CYAN, 0),
    }
    attrs = {}
    for n, (name, (fg, b, extra)) in enumerate(pairs.items(), start=1):
        curses.init_pair(n, fg, b)
        attrs[name] = curses.color_pair(n) | extra
    return attrs


def _draw(screen, game: Game, attrs: dict) -> None:
    height, width = screen.getmaxyx()
    screen.erase()
    need = min_height(game)
    if width < MIN_W or height < need:
        game.hits = []
        msg = f"enlarge the terminal to at least {MIN_W}x{need} (q quits)"
        screen.addstr(0, 0, msg[: width - 1], attrs["msg"])
        screen.refresh()
        return
    cells, game.hits = layout(game, width, height)
    for row, col, text, style in cells:
        if 0 <= row < height and 0 <= col < width - 1:
            try:
                screen.addstr(row, col, text[: width - 1 - col], attrs[style])
            except curses.error:
                pass
    screen.refresh()


def _read_escape(screen) -> Optional[str]:
    """After ESC: the rest of a CSI sequence, or None for a bare ESC."""
    screen.nodelay(True)
    chars = []
    try:
        while len(chars) < 32:
            c = screen.getch()
            if c == -1:
                break
            chars.append(chr(c))
            if len(chars) >= 2 and (chars[-1].isalpha() or chars[-1] == "~"):
                break
    finally:
        screen.nodelay(False)
        screen.timeout(1000)
    return "".join(chars) or None


def parse_sgr_mouse(seq: str):
    """'[<b;x;yM' -> (button, row, col, pressed) with 0-based row/col."""
    if not (seq.startswith("[<") and seq[-1] in "Mm"):
        return None
    try:
        b, xs, ys = seq[2:-1].split(";")
        return int(b), int(ys) - 1, int(xs) - 1, seq[-1] == "M"
    except ValueError:
        return None


def handle_mouse(game: Game, event) -> None:
    b, row, col, pressed = event
    if b & 64:                                   # wheel
        return
    if b & 32:                                   # motion while dragging
        return
    button = b & 3
    if pressed and button in (0, 2):
        game.mouse_down(row, col, button)
    elif not pressed and button == 0:
        game.mouse_up(row, col)


def _trace(line: str) -> None:
    """Opt-in input log for diagnosing terminals: SOLITAIRE_TUI_TRACE=file."""
    path = os.environ.get("SOLITAIRE_TUI_TRACE")
    if path:
        with open(path, "a", encoding="utf-8") as fh:
            fh.write(line + "\n")


def _main(screen, game: Game) -> None:
    curses.curs_set(0)
    attrs = _init_colors()
    screen.keypad(True)
    screen.timeout(1000)                         # tick the clock once a second
    sys.stdout.write(MOUSE_ON)
    sys.stdout.flush()
    try:
        while not game.quit:
            _draw(screen, game, attrs)
            ch = screen.getch()
            if ch in (-1, curses.KEY_RESIZE):
                continue
            if ch == 27:
                seq = _read_escape(screen)
                if seq is None:
                    game.key(27)
                else:
                    event = parse_sgr_mouse(seq)
                    if event:
                        handle_mouse(game, event)
                    _trace(f"esc {seq!r} -> {event} held={game.held} msg={game.message!r}")
                continue
            game.key(ch)
            _trace(f"key {ch} held={game.held} msg={game.message!r}")
    finally:
        sys.stdout.write(MOUSE_OFF)
        sys.stdout.flush()


def run(seed: Optional[int] = None, draw3: bool = False) -> None:
    locale.setlocale(locale.LC_ALL, "")
    os.environ.setdefault("ESCDELAY", "25")
    curses.wrapper(_main, Game.new(seed, draw3))
