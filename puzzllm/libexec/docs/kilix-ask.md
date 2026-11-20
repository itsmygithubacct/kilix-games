# Ask Kilix in ordinary language

Run `kilix ask` in the player shell to get a prompt, or put the request on the command line. Coding agents can use the one-line form in their shell tool. Put the request in quotes so the shell passes it intact.

```sh
kilix ask 'list panes'
kilix ask 'what time is it?'
kilix ask '17 * 3'
```

The pane list describes this game's private Kilix instance. Ask prints the answer in the terminal that called it. To ask several questions in a row, run `kilix ask --repl` and type `exit` when finished.

Ask can also plan pane actions through Needle. For example, substitute a current ID from `kilix` in `kilix ask 'focus pane 2'`. Read the proposed action before confirming it. For an exact command in an agent script, see the read and send examples in `~/docs/kilix.md`.

`kilix ask --help` shows the options for this installation.
