# Private Kilix panes

The coding harness and the player shell are panes of one Kilix that puzzllm
starts inside the sandbox. That Kilix is not the desktop instance. Its broker
runtime, listen socket, and frames stay under the puzzllm state directory, at
`/opt/puzzllm/kx` inside the sandbox.

Each pane has Kilix's clickable title bar. The shell pane is the tmux session
named `shell`, so `tmux capture-pane -p -t shell` still reads it. To type into
the shell from the agent pane, run:

```bash
kitten @ --to unix:/opt/puzzllm/kx/listen.sock send-text \
    --match env:PUZZLLM_PANE=shell --stdin
```

`puzzllm-kilix send shell` does the same and refuses any other listen socket.
The shell transcript is still appended to `~/shell.log`.
