import time
import unittest

from solitaire_tui import engine, tui
from solitaire_tui.engine import Move, State, card


def screen(game, rows=45, cols=90):
    grid = [[" "] * cols for _ in range(rows)]
    cells, game.hits = tui.layout(game, cols, rows)
    for row, col, text, _ in cells:
        for k, ch in enumerate(text):
            if 0 <= row < rows and 0 <= col + k < cols:
                grid[row][col + k] = ch
    return ["".join(r) for r in grid]


def centre(game, target):
    for y0, y1, x0, x1, t in reversed(game.hits):
        if t == target:
            return y0, (x0 + x1) // 2
    raise AssertionError(f"{target} not on screen")


def game_with(tableau, down=None, found=(0, 0, 0, 0), waste=(), stock=()):
    down = down or (0,) * 7
    return tui.Game(State(stock=stock, waste=waste, found=found,
                          tableau=tuple(tuple(p) for p in tableau), down=down))


class LayoutTest(unittest.TestCase):
    def test_cards_are_boxes_and_tops_are_readable(self):
        g = tui.Game.new(7)
        text = "\n".join(screen(g))
        self.assertIn("╭─────╮", text)      # ╭─────╮
        for i in range(7):
            label = tui.card_text(g.state.tableau[i][-1])
            self.assertIn(label, text)

    def test_compact_layout_when_short(self):
        g = tui.Game.new(7)
        tall = tui.layout(g, 90, 45)[0]
        short = tui.layout(g, 90, tui.min_height(g))[0]
        self.assertTrue(tall and short)


class MouseTest(unittest.TestCase):
    def test_click_stock_deals(self):
        g = tui.Game.new(7)
        screen(g)
        g.mouse_down(*centre(g, ("stock",)))
        self.assertEqual(len(g.state.waste), 1)

    def test_click_select_then_click_drop(self):
        # red 6 on column 1, black 7 on column 2: 6 onto 7 is legal
        six, seven = card(6, 1), card(7, 0)
        g = game_with([[six], [seven], [], [], [], [], []],
                      stock=tuple(c for c in range(52) if c not in (six, seven)))
        screen(g)
        y, x = centre(g, ("tab", 0, 0))
        g.mouse_down(y, x)
        g.mouse_up(y, x)
        self.assertEqual(g.held, ("tab", 0, 1))
        time.sleep(tui.DOUBLE_CLICK + 0.05)
        screen(g)
        y2, x2 = centre(g, ("tab", 1, 0))
        g.mouse_down(y2, x2)
        g.mouse_up(y2, x2)
        self.assertEqual(g.state.tableau[1], (seven, six))

    def test_drag_and_drop(self):
        six, seven = card(6, 1), card(7, 0)
        g = game_with([[six], [seven], [], [], [], [], []],
                      stock=tuple(c for c in range(52) if c not in (six, seven)))
        screen(g)
        g.mouse_down(*centre(g, ("tab", 0, 0)))
        g.mouse_up(*centre(g, ("tab", 1, 0)))
        self.assertEqual(g.state.tableau[1], (seven, six))

    def test_double_click_and_right_click_auto_place(self):
        ace = card(1, 0)
        rest = tuple(c for c in range(52) if c != ace)
        g = game_with([[ace], [], [], [], [], [], []], stock=rest)
        screen(g)
        y, x = centre(g, ("tab", 0, 0))
        g.mouse_down(y, x)
        g.mouse_up(y, x)
        g.mouse_down(y, x)                         # second click, same card
        self.assertEqual(g.state.found[0], 1)
        g = game_with([[ace], [], [], [], [], [], []], stock=rest)
        screen(g)
        g.mouse_down(*centre(g, ("tab", 0, 0)), button=2)
        self.assertEqual(g.state.found[0], 1)

    def test_buttons(self):
        g = tui.Game.new(7)
        screen(g)
        g.mouse_down(*centre(g, ("button", "deal")))
        self.assertEqual(len(g.state.waste), 1)
        screen(g)
        g.mouse_down(*centre(g, ("button", "undo")))
        self.assertEqual(g.state.waste, ())
        screen(g)
        g.mouse_down(*centre(g, ("button", "quit")))
        self.assertTrue(g.quit)

    def test_sgr_parsing(self):
        self.assertEqual(tui.parse_sgr_mouse("[<0;10;5M"), (0, 4, 9, True))
        self.assertEqual(tui.parse_sgr_mouse("[<0;10;5m"), (0, 4, 9, False))
        self.assertIsNone(tui.parse_sgr_mouse("[A"))


class AutoPlaceTest(unittest.TestCase):
    def test_prefers_foundation_then_revealing_build(self):
        ace = card(1, 2)
        g = game_with([[ace], [], [], [], [], [], []],
                      stock=tuple(c for c in range(52) if c != ace))
        g.area, g.pos = tui.TAB, 0
        self.assertEqual(g.auto_move(), Move(engine.TAB_FOUND, src=0))

        # a black 5 that can go on either red 6; the one that uncovers wins
        five, six_h, six_d, hidden = card(5, 0), card(6, 1), card(6, 2), card(9, 3)
        used = {five, six_h, six_d, hidden}
        g = game_with([[hidden, five], [six_h], [], [six_d], [], [], []],
                      down=(1, 0, 0, 0, 0, 0, 0),
                      stock=tuple(c for c in range(52) if c not in used))
        g.area, g.pos = tui.TAB, 0
        move = g.auto_move()
        self.assertEqual(move.kind, engine.TAB_TAB)
        self.assertIn(move.dst, (1, 3))
        g.auto_place()
        self.assertEqual(g.state.down[0], 0)       # the hidden card turned up

    def test_never_shuffles_a_lone_king_between_gaps(self):
        king = card(13, 0)
        g = game_with([[king], [], [], [], [], [], []],
                      stock=tuple(c for c in range(52) if c != king))
        g.area, g.pos = tui.TAB, 0
        self.assertIsNone(g.auto_move())


class KeyboardTest(unittest.TestCase):
    def test_deal_pickup_cancel_undo(self):
        g = tui.Game.new(7)
        g.key(ord("d"))
        self.assertEqual(len(g.state.waste), 1)
        g.key(ord("u"))
        self.assertEqual(g.state.waste, ())
        g.jump(0)
        g.key(ord(" "))
        self.assertEqual(g.held, ("tab", 0, 1))
        g.key(27)
        self.assertIsNone(g.held)


if __name__ == "__main__":
    unittest.main()
