# The Rename Carousel

Write `~/rename.sh`. The grader runs it with Bash in a fresh directory containing exactly four regular files named A, B, C and D. Its standard input is a four-line manifest. Each line contains a source and a destination separated by one space. Each name occurs once in each column.

For example, this manifest swaps A and B and leaves C and D in place.

```text
A B
B A
C C
D D
```

After Bash exits successfully, every destination must contain the original bytes of its source. The directory must again contain exactly the four regular files. You can use temporary names during the run. Files can contain arbitrary binary data. The checker tries all 24 permutations, including cycles of length four.
