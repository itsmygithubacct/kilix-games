# Inspect the player shell

The shell pane runs in the tmux session named `shell`. To read the current
screen from the coding agent, run:

```bash
tmux capture-pane -p -t shell
```

To include tmux scrollback, run:

```bash
tmux capture-pane -p -S - -t shell
```

The shell appends live output to `~/shell.log` across rooms and display
clears. If the file is removed, new output recreates it. Use
`tail -n 100 ~/shell.log` to see recent output after it scrolls out of view.
