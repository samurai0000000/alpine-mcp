# alpine-mcp design

This document describes how the add-on is built. For what it does and how to use it, read `README.md`.

## 1. Purpose and boundaries

Alpine is the human's mail reader and stays that. The add-on exports a small set of mail operations over the Model Context Protocol (MCP) so an AI agent can assist, using the running Alpine process and the logins it already holds. Three rules shape everything else:

1. **Parity with the human, no more.** What Alpine can reach with its configured accounts, the add-on can reach. It never gets a credential, never prompts, never starts a browser or device-code login.
2. **No expunge.** No expunge tool, no `CL_EXPUNGE` on close, and no handler ever removes messages from disk. Setting `\Deleted` and copying with `move: true` (which marks the source `\Deleted`) are allowed, but expunging messages is strictly outside MCP (subject to external Alpine settings or server autoexpunge policies).
3. **Untrusted input.** Message text is data. Nothing in a message is run, parsed for tool calls, or allowed to authorize a send.

With the add-on off (no `config.json`, or `mcp.enabled` false) the process is upstream Alpine 2.26: no socket, no extra thread, no change to the screen, the keyboard wait or the title.

## 2. Process model

```text
 client (any program)                    aimon gateway (optional)
     | one JSON-RPC message per line               ^ gateway/register
     v                                             | tools/call
 Unix socket -------------------------------+     |
                                           v       v
                              listener thread (one thread: accept,
                              read lines, parse, validate, enqueue)
                                           |  bounded queue (8), no c-client calls
                                           v
                       Alpine main thread: drain queue, run tools
                       pith / c-client, same thread as the TUI
```

- There is one c-client session. The human and the agent use different APIs in the same process, not two sessions.
- **Only the main thread calls pith or c-client.** The listener thread parses JSON, checks the operator lock and each tool's argument schema, enqueues, and writes one byte to a self-pipe. It owns the aimon connection too.
- A tool runs to completion once started. There is no abort: c-client has no safe way to cancel a command that leaves the human's session alone.

## 3. Protocol

Messages are UTF-8 JSON-RPC 2.0, one per line (MCP revision 2025-11-25, stdio framing), on two paths: a client to Alpine's Unix socket, or aimon to the gateway connection Alpine opens. Alpine implements `initialize`, `notifications/initialized`, `tools/list`, `tools/call` and `ping`. `serverInfo` is `alpine-mcp` / `2.26`. Request ids keep the JSON type the client sent (aimon sends strings).

| Code | Meaning |
|---|---|
| `-32700`, `-32600`, `-32601` | Not JSON, not a request, unknown method or tool |
| `-32602` | Arguments fail the closed schema; unlisted folder string; uid/uidvalidity mismatch; bad role |
| `-32603` | The operation failed inside pith or c-client |
| `-32001` | Operator lock is on (every `tools/call`, before validation) |
| `-32004` | The client left before the call started |
| `-32005` | Queue is full |
| `-32006` | Idempotency key pending, or reused with different arguments |
| `-32007` | The human answered no, or the question lapsed |
| `-32008` | The account has no login Alpine already holds |

A tool result is `{"content":[{"type":"text","text":"<json>"}],"isError":false}`; application errors use the JSON-RPC `error` member.

## 4. Queue, wake-up and cancellation

- The queue holds 8 calls; a ninth gets `-32005`.
- `input_ready()` (`pico/osdep/read.c`) is the single place every Alpine screen waits for a key, including the composer and modal screens. It watches stdin and, when MCP is on, the self-pipe. If only the pipe is readable it runs the drain and waits again with the remaining time. The same drain also runs from `read_command_prep` for work that arrived while Alpine was busy.
- A drain sets a re-entry flag. Tool code never calls `read_char`, `want_to`, a login prompt, `scrolltool`, `q_status_message` or `display_message`.
- If the client's socket closes, its queued and parked calls are marked cancelled; the drain answers `-32004` without starting them.
- A call that needs a human answer (section 8) is parked, and when that happens `input_ready` returns `NO_OP_COMMAND` once, as a timeout does, so the command loop runs and the question appears without eating a key. The composer and modal screens treat it as the timeout they already handle.

## 5. Non-interactive discipline

While a tool runs, a flag (`mcp_noninteractive()`) tells the callbacks Alpine and c-client use to behave:

- **Passwords:** `mm_login` uses only the password Alpine has already cached for that host; with none it records that the host needs the human and aborts.
- **OAuth 2.0 (XOAUTH2):** `mm_login_method` lets Alpine's own token loader run only when a refresh token for that account is already cached or in the password file, on the first try, with the account name in the connection spec (`mcp_oauth2_cached`, `alpine/imap.c`). Otherwise it refuses. A failed refresh is never retried and never deletes the human's cached token. No browser, device flow or question is started.
- **Messages and prompts:** `mm_notify`, `mm_log`, the TCP-timeout query, the certificate query and the SSL failure message record the text for the tool's error and paint nothing. `busy_cue` returns at once, so a long call does not draw "Busy" or "Sending mail" on the status line.

A login that is missing becomes `-32008` for that tool; other hosts are unaffected.

## 6. Folders, streams and messages

- A folder argument is the full c-client mailbox string that `mail_list_folders` returned in `mailbox` (for an IMAP folder it includes the `{host/flags}` prefix, so two accounts stay distinct). Each row also has a `name` (the part after the prefix, modified UTF-7 decoded to UTF-8, as Alpine's folder list shows it). A string the tool did not return is `-32602`.
- A folder that Alpine already has open (the one on screen, or any stream in its pool) is used as it is; its flags are not touched and it is not closed. Any other folder is opened on a pooled side stream (`SP_USEPOOL | SP_TEMPUSE | OP_SILENT`), never stored in `ps_global->mail_stream`, and closed after the call. The human's index and open message stay where they were.
- Messages are IMAP UIDs. `uid` and `uidvalidity` are both required; a mismatch is refused, so an old id cannot reach another message. Sequence numbers are never arguments.
- `mail_get_message` fetches with `FT_PEEK` and the UID flag, so reading does not set `\Seen`. It decodes the transfer encoding and charset to UTF-8, replaces invalid bytes, and every string that came from a message is made valid UTF-8 before it enters JSON.

## 7. Tools

| Tool | pith / c-client |
|---|---|
| `mail_list_folders` | `pine_mail_list` over each collection, plus the configured incoming folders |
| `mail_folder_status` | open stream: counts from the stream and a server-side unseen search; otherwise `pine_mail_status`; accepts `folder` (single) or `folders` (1..32 unique folders) with per-folder status results |
| `mail_list_messages` | fast fetch and overview for the newest messages |
| `mail_search` | `pine_mail_search_full` with a `SEARCHPGM` built from the closed argument set (no raw criteria) |
| `mail_get_message` | `pine_mail_fetch_structure`, `pine_mail_fetch_body` |
| `mail_search_addressbook` | open address books, or a private handle on a local book; a remote one that is not open is reported `not_open` |
| `mail_copy` | accepts `uid` or `uids` (1..250 unique UIDs); same server: single `UID COPY` sequence set with flag `CP_UID` (and `CP_MOVE` added when `move` is true); cross-server append path: sequential per-message append capped at 25 UIDs and 64 MB total (sources marked `\Deleted` only after successful append if `move` is true); pre-release read-back confirms source state |
| `mail_set_flag` | accepts `uid` or `uids` (1..250 unique UIDs); `mail_flag` with the UID flag (`\Seen`, `\Flagged`, `\Answered`, `\Deleted`); batch issues a single compressed sequence set `UID STORE`; convergent read-back with at most one re-issue |
| `mail_create_draft` | `write_postponed` to the configured `postponed-folder` |
| `mail_send` | `call_mailer` |

Arguments are validated on the listener thread against a table of allowed fields; unknown fields are refused. `to`, `cc`, `bcc`, `subject` and `from` may not contain control characters or line breaks.

### Batch operations and convergent semantics

- **Limits**: `mail_set_flag` and `mail_copy` accept `uids` (1..250 unique positive integers). `mail_folder_status` accepts `folders` (1..32 unique mailbox strings). Exactly one of `uid`/`uids` and `folder`/`folders` must be supplied. The 250 UID cap keeps sequence sets well bounded in stack buffers, and the 32 folder cap prevents excessive serialized STATUS latency.
- **Same-server batching**: Same-server batch copy and flag operations construct an IMAP sequence set (`mcp_uid_set`). A call sends a `SEARCH` and a `UID FETCH FLAGS` to resolve, one `UID STORE` or `UID COPY` per 1000 characters of sequence set, then the read-back pair (`SEARCH` and `UID FETCH FLAGS`).
- **Convergent policy**: A batch operation succeeds when final folder state matches the requested state. Missing UIDs (messages expunged or missing from the folder) are skipped and reported in `missing_uids` rather than failing the entire batch; an error (`-32602`) is returned only if *no* requested UID resolves. Flag operations are sent unconditionally for all resolved UIDs (never skipped on cached state), followed by fresh post-command read-back; messages whose flags still differ receive at most one re-issue. Copy operations are never re-issued to prevent duplicates.
- **Cross-server append exception**: Copying across distinct mail servers cannot use IMAP `UID COPY` and instead fetches and appends each message individually. Cross-server batches are strictly capped at 25 UIDs (`MCP_APPEND_BATCH_MAX`) and 64 MB total payload size (`MCP_APPEND_MAX`) to prevent freezing the Alpine process. The operation halts at the first append failure without rolling back earlier copies. With `move: true`, sources are marked `\Deleted` in one batch only after their appends succeed.
- **Multi-folder status**: `mail_folder_status` with `folders` produces `{"statuses":[...]}` in requested order. Any individual folder error (e.g. unknown mailbox, unauthenticated server `-32008`) is recorded in that folder's object under `error`, allowing valid folders to succeed.

## 8. Policy

$HOME/.config/alpine-mcp/config.json, mode `0600`, is the only policy store. It is read at start and rewritten atomically (temporary file, `fsync`, rename) by the monitor screen. Defaults are fail-closed: off, unlocked, empty allow list.

- **Operator lock.** Checked on the listener thread before anything else. When on, every `tools/call` returns `-32001`; `initialize`, `tools/list` and `ping` still answer.
- **Sending.** A repeat of a finished send is answered from its record. If every recipient (to, cc, bcc) matches the allow list (`local@domain` or `*@domain`) the message is sent. Otherwise the call is parked and, from `read_command_prep`, Alpine asks once with `want_to` (default no). Yes runs the send; no, a vanished client, a lock turned on meanwhile, or (for aimon calls, which aimon abandons after 45 s) an answer later than 40 s ends the call with `-32007` or `-32001` and sends nothing. The answer is not remembered. A client that disconnects while the question is on screen still cancels the call.
- **Roles.** Optional `from` names a role whose compose action is "Without confirmation" and that sets a From. The role supplies the From address and, for a send, its SMTP server, for that call only. The role selected in the TUI is not changed.

## 9. Idempotency

`mail_create_draft` and `mail_send` take a key (8 to 128 of `[A-Za-z0-9._-]`). Records live in `$XDG_STATE_HOME/alpine-mcp/idempotency` or `$HOME/.local/state/alpine-mcp/idempotency` (directory `0700`, files `0600`) and hold the tool name, the SHA-256 of the canonical arguments, a state and, when done, the result. The sequence is: write `pending` and `fsync` before the effect, replace with `done` after. The same key and arguments return the stored result; different arguments, or a `pending` record, are `-32006`. A failure that proves nothing happened removes the record. An off-list send writes nothing until the human says yes. The monitor screen can delete one pending record.

## 10. Building a message

Draft and send share one builder: the From (role or account default), the envelope sender (return path) set to that same From address as the composer does, the Date, a Message-ID, the subject, and a text body. Without the envelope sender c-client would send `MAIL FROM:<>`, which receiving servers treat as a bounce and may drop silently. The body is prepared the way the composer prepares one (encoding and charset sniffed, marked as edited UTF-8) and, when it is not plain ASCII, written as UTF-8 even if Alpine runs in the C locale. No attachments. No copy is filed in a sent folder.

## 11. Monitor screen

Setup, then `i` (monItor), a scroll screen of plain text: lock state, allow-list size, socket path, aimon state, pending records, last error. `L` and `U` change the lock, `W` edits the allow list, `A` toggles aimon, `R` drops the aimon connection so it registers again, `P` removes the first pending record after a yes/no, `E` leaves. Every change goes through the same atomic write of `config.json`.

## 12. aimon gateway

When `mcp.enabled` and `aimon.enabled` are both true, the listener thread connects to the gateway (default `127.0.0.1:3885`), sends `gateway/register` as the subsystem `alpine-mcp` with the current tool list, and waits for `registered`. Inbound `tools/call` goes through the same validation and queue as the socket. On a failed or refused registration, EOF, or a five-second connect failure it retries after 1, 2, 4, 8, 16, then 30 seconds. Nothing is painted on the screen when the gateway is down. The tool list is re-sent by reconnecting.

## 13. Source map

All Alpine changes are on the branch `alpine-mcp` of the submodule (Alpine 2.26, commit `9726c098`, not pushed).

| Where | Change |
|---|---|
| `pith/mcp/` (new) | `mcpserv.c` listener, queue, aimon client, drain, parking; `mcptools.c` tool table and validators; `mcpmail.c` folder listing and the login cache helpers; `mcpread.c` read tools; `mcpwrite.c` copy, flag, draft, send; `mcpidem.c` idempotency store and SHA-256; `mcpconf.c` policy file; `mcppath.c` socket path; `cJSON.[ch]` (v1.7.19, MIT), `mcpjson.[ch]`, headers |
| `alpine/mcpui.[ch]` (new) | drain entry points, the off-list question, the monitor screen |
| `pico/osdep/read.[ch]` | the wake descriptor, run and return hooks in `input_ready` |
| `alpine/alpine.c`, `keymenu.[ch]` | Setup `i` (monItor) |
| `alpine/titlebar.c` | `ALPINE(MCP)` (or `ALPINE?MCP?` on error) with timer-driven middle-character clockwise spinner (`\` `\|` `/` `-`), contrasting badge cycling, and isolated delimiters |
| `alpine/osdep/termin.gen.c` | drain and question from `read_command_prep` |
| `alpine/imap.c`, `pith/imap.[ch]` | the non-interactive callback guards and token reuse |
| `alpine/busy.c` | no busy cue while a tool runs |
| `alpine/Makefile.*`, `pith/Makefile.*` | the new sources |

Every addition is inactive when MCP is off.

## 14. Testing

All tests are CppUTest, written in C++ (`.cxx`/`.hxx`), under `test/`, run by the top-level `Makefile`, and write only below `build/`.

**Unit tests** (`make test-unit`, `test/unit`) link the add-on's own C files without the rest of Alpine: the server (socket, queue, framing, parking, the aimon client against a fake gateway on loopback), the policy file, the idempotency store, SHA-256, the validators and the socket path. They cover boundaries (line, queue and string limits, numeric ranges, dates, keys) and inject faults: the C files are built with a counting allocator, `-Wl,--wrap` lets a test fail the Nth `open`, `write`, `fsync`, `rename`, `mkdir`, `socket`, `bind`, `listen`, `pipe2` or `pthread_create`, cJSON's allocator can fail, and time can be advanced. Tests check that every failure leaves no record, no descriptor and no leak behind.

**Integration tests** (`make test-integration`, `test/integration`) run the real Alpine under a pseudo terminal with a home of its own (pinerc, policy file, local mbox folders, address book) and a loopback SMTP sink as its mail host. They drive it as a person and as an agent would: MCP calls on the socket, keys on the terminal. They cover the protocol and the policy file (unsafe modes, broken JSON, a second Alpine), every tool on real folders, the lock, the allow list and every way of writing an outsider's address, the question on screen (it appears before any key, y, n, Enter and ^C, other keys ignored, not remembered, a client that leaves), what goes on the wire (envelope sender, headers, no Bcc, UTF-8, dot-stuffing), retries and replays across a restart, an SMTP host that refuses or is down, and the monitor screen. The harness is isolated, and the test binary checks it: it compares the real home's mail files, pinerc, address book and MCP directories before and after, and fails the run if anything changed (Alpine takes its home from the password file, not from `$HOME`, so folder names must be absolute).

**Coverage** (`make coverage`) runs both suites on instrumented code, an `-O0 --coverage` Alpine for the integration run, and merges them with lcov into `build/coverage` (HTML and `summary.txt`). The required files, those that link without Alpine, must reach 85% of lines and the whole add-on 75%. The files that need a real IMAP server (the remote paths of `mcpmail.c`, `mcpwrite.c`, `mcpread.c`) or a person (parts of the monitor in `mcpui.c`) are reported, not required.

**Mutation check** (`scripts/mutation-check.sh`) breaks the code on purpose, one defect at a time (the validator accepts a non-enum flag `\Draft`, copy with `move=false` passes `CP_MOVE`, an empty envelope sender, a cancelled question that still sends, a dropped `fsync`), and requires the matching tests to fail, then restores the sources.

Real accounts are exercised by hand, not by the suites: reads, copies, flags, drafts and sends in both directions, including the OAuth account. The evidence for each stage is kept in the working tree's `plan/` directory, which is not part of the repository.

## 15. Known limits and what is not built

- Folders on a server whose login Alpine does not hold return `-32008`; `mail_search` covers one folder; attachments are listed, never returned; remote address books must be open.
- Copy or move between two servers uses the per-message append path (capped at 25 UIDs and 64 MB per call); batch copy and batch move with post-append source deletion have been verified live with batches of up to 3 messages.
- Not covered by any automated test or live run: the read-only guard (`s->rdonly`) in `mail_set_flag` and `mail_copy` (an unwritable local mailbox is refused at open for every tool, so it is only reachable for IMAP mailboxes opened read-only); the 64 MB total cap and the stop-at-first-failure behavior of the cross-server batch path.
- Leak checking is not part of the test suites: the unit tests use CppUTest's allocator hooks, which valgrind cannot run cleanly.
- The tool reports the hand-off to the SMTP server, not delivery, and does not record the server's reply line.
- Not built: the later layer for authenticated agent-to-agent collaboration carried in mail (it would be its own protocol, never an MCP tool that reads message bodies), a headless daemon, classification and summaries (the agent does those with the read tools), and anything that expunges or changes folder structure.
