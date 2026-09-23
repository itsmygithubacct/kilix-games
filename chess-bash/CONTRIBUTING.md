# Contributing

Bug reports and focused pull requests are welcome.

When reporting a display problem, include the terminal name and version, Linux
distribution, terminal dimensions, whether SSH or a multiplexer is involved,
and a screenshot when possible. For audio issues, include the result of
`./chess-bash --sound-test`.

Build and test changes before submitting them:

```sh
make clean
make
make test
```

The project uses warning-clean C11, four-space indentation, and braces for
multi-line control flow. Keep platform-specific behavior isolated, avoid adding
runtime library dependencies without a clear benefit, and add deterministic
coverage for rule or engine changes.

Generated render-test files and local binaries are intentionally ignored. Do
not commit private paths, credentials, generated research archives, or copied
third-party game artwork.
