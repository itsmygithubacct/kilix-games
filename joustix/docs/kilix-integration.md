# Kilix 95 integration

Joustix runs directly in Kilix; no graphics bridge is necessary. Current
Kilix 95 releases include Joustix in Start ▸ Programs ▸ Games. The first
launch clones the pinned source from
`https://github.com/itsmygithubacct/joustix`, builds it under Kilix's user data
directory, records the executable in `games.conf`, and opens it in a new tab.

For a personal checkout, build and install the executable directly:

```sh
git clone https://github.com/itsmygithubacct/joustix.git
cd joustix
make
make install PREFIX="$HOME/.local"
```

An explicitly configured local build is also supported. Point the `joustix`
section of `~/.config/kilix/games.conf` at its directory:

```ini
[joustix]
dir = /path/to/joustix
```

Kilix treats that path as a user-managed executable. Its managed download,
however, remains pinned to an immutable source commit.

When running Kilix through tmux or SSH, ensure Kitty graphics escape sequences
are forwarded unchanged. Test the game in a direct Kilix tab first if the
graphics probe does not receive a reply.
