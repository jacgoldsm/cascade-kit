"""A minimal Cascade engine in Python: plays a uniformly random legal move.

It shows the protocol (PROTOCOL.md) and the board geometry (RULES.md sections 1
and 7) in a language other than JavaScript. Standard library only.
"""
import random
import sys

DIRS = [("E", 1, 0), ("NE", 1, -1), ("NW", 0, -1), ("W", -1, 0), ("SW", -1, 1), ("SE", 0, 1)]


class Board:
    def __init__(self, side):
        r_max = side - 1
        self.rows = []  # rows[k] = list of (q, r) cells, left to right
        for r in range(-r_max, r_max + 1):
            q_min, q_max = max(-r_max, -r_max - r), min(r_max, r_max - r)
            self.rows.append([(q, r) for q in range(q_min, q_max + 1)])
        self.names = {}
        for k, row in enumerate(self.rows):
            for i, cell in enumerate(row):
                self.names[cell] = "abcdefghijklmnopqrstuvwxyz"[k] + str(i + 1)

    def legal_moves(self, csn):
        """Legal moves in a CSN position, as strings like 'e5NE'."""
        board, side_to_move = csn.split()[:2]
        stacks = {}
        for row_cells, row_text in zip(self.rows, board.split("/")):
            for cell, stack in zip(row_cells, row_text.split(",")):
                stacks[cell] = "" if stack == "-" else stack
        moves = []
        for (q, r), stack in stacks.items():
            if stack and stack[-1] == side_to_move:  # top piece is ours
                for name, dq, dr in DIRS:
                    if (q + dq, r + dr) in stacks:  # first step must stay on the board
                        moves.append(self.names[(q, r)] + name)
        return moves


def main():
    board = Board(5)
    position = None
    for line in sys.stdin:
        cmd, _, rest = line.strip().partition(" ")
        if cmd == "cascade":
            print("name python-random")
            print("ready", flush=True)
        elif cmd == "newgame":
            options = dict(tok.split("=") for tok in rest.split())
            board = Board(int(options.get("side", 5)))
        elif cmd == "position":
            position = rest
        elif cmd == "go":
            print("bestmove " + random.choice(board.legal_moves(position)), flush=True)
        elif cmd == "quit":
            break


if __name__ == "__main__":
    main()
