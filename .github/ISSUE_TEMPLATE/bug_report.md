---
name: Bug report
about: A client's screen differs from the session, an attach or detach misbehaves, the server or ssh transport fails, or a test fails
labels: bug
---

**What you did** (the `bromux` commands or `Client` calls, in order: server
start, attach, resize, detach, reconnect; local or `--ssh`):

```
```

**What should have happened:**

**What happened instead** (the client's screen against the session's, the
error, a hang, a crash, a server or helper process left behind, or the failing
`ctest --output-on-failure` output — paste it):

```
```

**If you can, a recording:** start the server with `--tee-dir DIR` and attach
the session's `.tee` file; it replays the exact bytes the session saw.

**Environment:**
- OS of the client, and of the server if remote:
- ssh client and server versions, for remote attach:
- Compiler / toolchain (MSVC / GCC / Clang):
- bromux commit, and bropty / brosearch commits if built from siblings:
