# Ask Kilix in ordinary language

Run `kilix ask` in the player shell to get a prompt, or put the request on the command line. Coding agents can use the one-line form in their shell tool. Put the request in quotes so the shell passes it intact.

```sh
kilix ask 'list panes'
kilix ask 'what time is it?'
kilix ask '17 * 3'
```

The pane list describes this game's private Kilix instance. Ask prints the answer in the terminal that called it. To ask several questions in a row, run `kilix ask --repl` and type `exit` when finished.

Ask can also plan actions through Needle. Read the proposed action before confirming it. Needle resolves pane actions within the issuing terminal window. The coding pane and player shell each have their own window. To focus the other game pane, use its ID directly, for example `kilix pane focus pane:2`. See `~/docs/kilix.md` for reading and sending text between them.

`kilix ask --help` shows the options for this installation.
