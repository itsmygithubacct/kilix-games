# Control the game panes with kilix

Run `kilix` in the player shell or in the coding agent's shell tool to list the panes in this game. The coding terminal and the player shell are real Kilix panes. The game hides their tab and title bars. Both share the sandbox home.

```sh
kilix
kilix pane list --json
```

Use the ID printed for the pane you want. For example, if the list shows the player shell as pane 2, read its visible text or ask it to run a command with the following commands.

```sh
kilix pane read pane:2
kilix pane send pane:2 'pwd' --submit
kilix pane read pane:2 --extent all
```

Omit `--submit` to leave the text at the prompt for editing. The shell transcript is also available at `~/shell.log`. An agent that restarts can get a new pane ID. Run `kilix` again to find it.

Codex's restricted command shell can list, read and focus the game panes. Type commands directly in the player shell or use the local coding agent when you want to send text between panes.

For a direct remote-control command, the game assigns each pane a stable role. This reads the player shell by role.

```sh
kilix @ get-text --match 'env:^PUZZLLM_PANE$=^shell$'
```

See `~/docs/kilix-ask.md` for natural-language commands.
