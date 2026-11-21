# The Damaged Radio

Write a standalone Python 3 decoder in `~/radio.py`. Each input line is a seven-bit word. Print the original four bits, including leading zeros, on one line. Read until EOF. The checker sends each of the sixteen encoded messages undamaged and with each single bit flipped, covering all 128 received words.

Positions 1 to 7 run from left to right. The data bits occupy positions 3, 5, 6 and 7 in message order. The parity groups (1,3,5,7), (2,3,6,7) and (4,5,6,7) each have an even number of ones in an undamaged word. For example, both `0000000` and `0000001` decode to `0000`.

| Message | Transmitted word |
| --- | --- |
| `0000` | `0000000` |
| `0001` | `1101001` |
| `0010` | `0101010` |
| `0011` | `1000011` |
| `0100` | `1001100` |
| `0101` | `0100101` |
| `0110` | `1100110` |
| `0111` | `0001111` |
| `1000` | `1110000` |
| `1001` | `0011001` |
| `1010` | `1011010` |
| `1011` | `0110011` |
| `1100` | `0111100` |
| `1101` | `1010101` |
| `1110` | `0010110` |
| `1111` | `1111111` |
