# alpine-mcp

**Alpine stays your mail reader. alpine-mcp adds a local MCP interface to it, so an AI agent can help with your mail while you keep using Alpine as usual.**

alpine-mcp is [Alpine](https://alpineapp.email/) 2.26 with an add-on, kept on a branch of a git submodule (`third_party/alpine`). The add-on lets an agent that speaks the [Model Context Protocol](https://modelcontextprotocol.io/) list folders, search, read, file copies, flag, write drafts and, under rules you set, send mail. It does this inside the same running Alpine process, with the same logged-in accounts, folders and roles you already use. You still read, answer, move and delete mail in Alpine.

With the add-on switched off, or with no config file, the program behaves as upstream Alpine 2.26.

```text
  agent or other program              aimon gateway (optional)
       |  Unix socket                         ^
       |  (JSON-RPC, one per line)            |  tools/call
       +--------------------+-----------------+
                            v
        Alpine (the program you already use)
          human: the TUI, unchanged
          agent: semantic mail tools, on the same session
```

## What an agent can and cannot do

- It acts as you, through your session. If Alpine can reach an account, the add-on can, and nothing else. The agent never sees a password or a token. Alpine does the login, from the logins your session already holds.
- It never prompts and never starts a browser or device-code login. If an account has no login your session already holds (a password Alpine has used, or an OAuth refresh token it has saved), the tool answers `-32008` and you log in on the Alpine screen.
- It cannot expunge. There is no expunge tool and no handler ever removes messages from disk. The agent can set `\Deleted` and use `move: true` in `mail_copy` (which marks the source `\Deleted` without expunging). Physical expunging is strictly outside MCP (though Alpine settings or server autoexpunge policies may expunge `\Deleted` mail later).
- Message text is data, not instructions. Nothing in a message is ever run, and reading a message never allows a send.
- You can stop all of it with one switch, the operator lock.

## Build

Prerequisites: a C compiler, `make`, `rsync`, `python3`, OpenSSL (1.0.0c or later) and the usual Alpine build dependencies. Running the tests also needs CppUTest (`libcpputest-dev`), and `make coverage` needs `lcov`. Do not build with `cmake`; this is Alpine's own autotools build, wrapped by the top-level `Makefile`.

```bash
git submodule update --init third_party/alpine   # make does this if you forget
make -j8                                         # Alpine, all under build/
build/alpine/alpine/alpine -version              # prints a line starting "Alpine 2.26"
make test                                        # unit and integration tests
make coverage                                    # both, instrumented; report in build/coverage/
make help                                        # every target
```

Everything the build makes goes under `build/`, which git ignores, and the submodule stays clean. Alpine's bundled c-client library cannot build out of tree, so `make` copies the submodule's tracked and new files (uncommitted work included) into `build/alpine` and builds there; a second run rebuilds only what changed. `make distclean` removes `build/`.

The program is `build/alpine/alpine/alpine`; If you built in the submodule by hand earlier, `make clean-intree` removes those products.

## Turn it on

The add-on reads `$HOME/.config/alpine-mcp/config.json`. The file must be mode `0600`; a file that other users can read is rejected. A missing, unreadable or invalid file means everything below is off.

```json
{
  "mcp":            { "enabled": true },
  "operator_lock":  false,
  "allowed_recipients": ["me@example.org", "*@example.net"],
  "aimon":          { "enabled": false, "host": "127.0.0.1", "port": 3885 }
}
```

| Key | Default | Meaning |
|---|---|---|
| `mcp.enabled` | `false` | Opens the socket and turns on everything below. Off: upstream Alpine. |
| `operator_lock` | `false` | When `true`, every `tools/call` is refused with `-32001`, reads included. Toggle it on the monitor screen. |
| `allowed_recipients` | `[]` | Addresses a send may go to without asking you. `local@domain` or `*@domain`. Empty means nothing is sent unasked. |
| `aimon.enabled` | `false` | Also register the tools with an aimon gateway (below). |
| `aimon.host`, `aimon.port` | `127.0.0.1`, `3885` | Where the gateway listens. |

Your Alpine accounts, folders and roles stay in `~/.pinerc`; nothing from this file is written there.

Run Alpine as you always do, for example `build/alpine/alpine/alpine`. The title bar then reads `ALPINE(MCP)` (or `ALPINE?MCP?` if `config.json` is misconfigured).

## Connect an agent

There are two ways to reach the tools. Both carry MCP (revision 2025-11-25) as JSON-RPC 2.0, one message per line.

- **Unix socket.** Connect a program to `$XDG_RUNTIME_DIR/alpine-mcp.sock` (or `/tmp/alpine-mcp.<hostname>.sock` when that variable is not set) and exchange lines. The socket is private to your user. There is no stdio bridge in this repository; a client that only speaks stdio needs one of its own.
- **aimon gateway.** Set `aimon.enabled` to `true` and restart Alpine. It registers with the gateway as the subsystem `alpine-mcp`, and aimon calls the tools.

Only one Alpine can serve the socket. A second one starts normally, shows "another Alpine is serving ..." on its screen, and leaves the socket to the first.

## Tools

Folders are named by the full mailbox string that `mail_list_folders` returns (`mailbox`); the `name` field is the same folder as Alpine shows it, with IMAP's encoded names decoded. Messages are named by `uid` plus `uidvalidity`. A mismatch is refused, so a stale id never touches another message.

| Tool | What it does |
|---|---|
| `mail_list_folders` | Every folder in every collection and every incoming folder, with `mailbox`, `name`, collection, and whether it is an incoming folder. |
| `mail_folder_status` | Message, recent and unseen counts and `uidvalidity` of one folder. |
| `mail_list_messages` | The newest messages of a folder, newest first, 1 to 100: uid, flags, size, date, sender, subject. |
| `mail_search` | One folder. `from`, `to`, `subject`, `text`, `since`, `before` (YYYY-MM-DD), `seen`, `flagged`, `unanswered`, `limit`. No raw criteria string. |
| `mail_get_message` | Headers, the text body (first plain-text part, else HTML; at most 262144 bytes) and the attachment list (names and sizes only). Reading does not mark the message seen. |
| `mail_search_addressbook` | Entries of the address books Alpine has open or can read without a login. |
| `mail_copy` | Files a copy of a message in another folder (optional `"move": true` marks the source `\Deleted`). Never expunges. |
| `mail_set_flag` | Sets or clears `\Seen`, `\Flagged`, `\Answered`, or `\Deleted`. |
| `mail_create_draft` | Writes a message to your `postponed-folder` for you to review and send. Nothing is sent. |
| `mail_send` | Sends one message. Needs `to` and an `idempotency_key`. See below. |

`mail_create_draft` and `mail_send` take an `idempotency_key` (8 to 128 characters of letters, digits, `.`, `_`, `-`). The same key with the same arguments returns the first result and does nothing more. The same key with different arguments, or a key left `pending` by an attempt that never finished, is refused with `-32006`. Both also take an optional `from`: the nickname of a role whose compose action is "Without confirmation". It sets the From address, and for `mail_send` the SMTP server, for that one message only. It does not change the role you have selected.

### Sending

- Every recipient (to, cc and bcc) on `allowed_recipients`: the message goes out at once.
- Any other recipient: Alpine asks you once, on the status line, "Let the agent send mail to ... (not on the allow list)?", default No. Yes sends it. No returns `-32007`. A yes does not add the address to the list; the next send asks again.
- The question appears as soon as Alpine is waiting for a command (you are never asked in the middle of typing a message; it waits until you are back at a command screen). If the agent disconnects meanwhile, a later yes sends nothing.
- No attachments, and no copy is filed in a sent folder.

## What you see in Alpine

The screens are exactly upstream Alpine's, with the same number of lines. Only two things show the add-on is on:

- **Title bar:** `ALPINE(MCP)` (or `ALPINE?MCP?` if `config.json` is misconfigured). Text color indicates aimon connectivity (green when connected, red when disconnected). When MCP activity occurs, a 0.5s timer rotates the middle character (`C` -> `\` -> `|` -> `/` -> `-`) and cycles the inner badge through contrasting background/foreground colors, returning to the standard title bar background after 3 seconds of inactivity. Delimiters remain standard title bar text and background.
- **Monitor screen:** Setup, then `i` (monItor). It shows the lock, the allow-list size, the socket, the aimon state, pending idempotency records and the last error. Keys: `L` lock, `U` unlock, `W` edit the allow list, `A` aimon on or off, `R` reconnect aimon, `P` remove the first pending record (asks first), `E` leave.

## Limits you should know

- Folders are read through logins Alpine already holds. An account Alpine could not log in to on its own (nothing saved, a login that needs you) answers `-32008`.
- `mail_search` searches one folder; repeat it per folder.
- Attachments are never returned or opened, only listed.
- A send whose SMTP server needs a login that Alpine does not hold answers `-32008`.
- If Alpine reaches the account but the account's server drops mail, the tool cannot tell: it reports the hand-off, not delivery.
- A remote address book that is not open is reported as `not_open`.
- `mail_copy` (copy or move) between two servers is implemented but has not been exercised on real accounts.

## Not built

Email-based collaboration between agents on different hosts is a separate, later layer. It would be its own authenticated protocol and never an MCP tool that interprets message bodies. A headless daemon, message classification and summaries (agents do those with the read tools), and any expunge, folder or address-book change are out of scope.

## Repository layout

| Path | Contents |
|---|---|
| `third_party/alpine` | Alpine 2.26 (upstream tag, commit `9726c098`), branch `alpine-mcp`. The add-on is in `pith/mcp/` and `alpine/mcpui.c`. |
| `Makefile` | The one way to build and test; output in `build/`. |
| `test/` | CppUTest suites: `unit/` (the add-on without Alpine), `integration/` (a real Alpine on a pty), `support/` (fault injection, fakes). |
| `scripts/` | `covcheck.py` (coverage thresholds) and `mutation-check.sh` (shows the tests catch deliberate defects). |
| `Design.md` | How it works. |

## Copyright and licenses

Everything outside `third_party/alpine` is Copyright (C) 2026, Charles Chiou, with no license grant.

Inside `third_party/alpine`, Alpine's license applies: the Apache License 2.0 (see the `LICENSE` file there). That includes the add-on sources in `pith/mcp/` and `alpine/mcpui.[ch]`, which carry the copyright line and the Apache notice. `pith/mcp/cJSON.[ch]` is cJSON 1.7.19 (MIT).
